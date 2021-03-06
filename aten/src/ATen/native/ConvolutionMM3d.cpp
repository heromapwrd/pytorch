#include <ATen/ATen.h>
#include <ATen/Dispatch.h>
#include <ATen/Parallel.h>
#include <ATen/TensorUtils.h>
#include <ATen/core/grad_mode.h>
#include <ATen/div_rtn.h>
#include <ATen/native/Unfold3d.h>

constexpr int64_t CONV3D_GRAIN_SALT = 20;

namespace at {
namespace native {

namespace {

static inline void slow_conv3d_shape_check(
    const Tensor& input,
    const Tensor& grad_output,
    const Tensor& weight,
    const Tensor& bias,
    int64_t kernel_depth,
    int64_t kernel_height,
    int64_t kernel_width,
    int64_t stride_depth,
    int64_t stride_height,
    int64_t stride_width,
    int64_t pad_depth,
    int64_t pad_height,
    int64_t pad_width,
    bool weight_optional) {
  TORCH_CHECK(
      kernel_width > 0 && kernel_height > 0 && kernel_depth > 0,
      "kernel size should be greater than zero, but got: ",
      kernel_depth,
      " x ",
      kernel_height,
      " x ",
      kernel_width,
      " (TxHxW)");
  TORCH_CHECK(
      stride_width > 0 && stride_height > 0 && stride_depth > 0,
      "stride should be greater than zero, but got: ",
      stride_depth,
      " x ",
      stride_height,
      " x ",
      stride_width,
      " (TxHxW)");
  if (weight.defined()) {
    TORCH_CHECK(
        weight.numel() > 0 && (weight.dim() == 2 || weight.dim() == 5),
        "non-empty 2D or 5D weight tensor expected, but got: ",
        weight.sizes());
    if (bias.defined()) {
      check_dim_size(bias, 1, 0, weight.size(0));
    }
  } else {
    TORCH_CHECK(weight_optional, "weight tensor is undefined");
  }

  const int64_t ndim = input.dim();
  const int64_t dim_batch = 0;
  const int64_t dim_planes = 1;
  const int64_t dim_depth = 2;
  const int64_t dim_height = 3;
  const int64_t dim_width = 4;

  // Allow for empty batch size but not other dimensions
  bool valid_empty = ndim == 5 && input.size(dim_batch) == 0 &&
      input.size(dim_planes) != 0 && input.size(dim_depth) != 0 &&
      input.size(dim_height) != 0 && input.size(dim_width) != 0;

  TORCH_CHECK(
      (input.numel() > 0 || valid_empty) && ndim == 5,
      "non-empty 5D input tensor expected but got: ",
      input.sizes());

  const int64_t input_depth = input.size(dim_depth);
  const int64_t input_height = input.size(dim_height);
  const int64_t input_width = input.size(dim_width);

  const int64_t exact_input_depth = input_depth + 2 * pad_depth;
  const int64_t exact_input_height = input_height + 2 * pad_height;
  const int64_t exact_input_width = input_width + 2 * pad_width;

  TORCH_CHECK(
      exact_input_depth >= kernel_depth &&
          exact_input_height >= kernel_height &&
          exact_input_width >= kernel_width,
      "Calculated padded input size per channel: (",
      exact_input_depth,
      " x ",
      exact_input_height,
      " x ",
      exact_input_width,
      "). ",
      "Kernel size: (",
      kernel_depth,
      " x ",
      kernel_height,
      " x ",
      kernel_width,
      "). Kernel size can't be greater than actual input size");

  const int64_t output_depth =
      div_rtn<int64_t>(exact_input_depth - kernel_depth, stride_depth) + 1;
  const int64_t output_height =
      div_rtn<int64_t>(exact_input_height - kernel_height, stride_height) + 1;
  const int64_t output_width =
      div_rtn<int64_t>(exact_input_width - kernel_width, stride_width) + 1;

  TORCH_CHECK(
      output_depth >= 1 && output_width >= 1 && output_height >= 1,
      "Given input size per channel: (",
      input_depth,
      " x ",
      input_height,
      " x ",
      input_width,
      "). "
      "Calculated output size per channel: (",
      output_depth,
      " x ",
      output_height,
      " x ",
      output_width,
      "). Output size is too small");

  if (weight.defined()) {
    int64_t n_input_plane = weight.size(1);
    if (weight.dim() == 2) {
      n_input_plane /= (kernel_height * kernel_width);
    }
    check_dim_size(input, ndim, dim_planes, n_input_plane);
  }

  if (grad_output.defined()) {
    if (weight.defined()) {
      int64_t n_output_plane = weight.size(0);
      check_dim_size(grad_output, ndim, dim_planes, n_output_plane);
    } else if (bias.defined()) {
      TORCH_CHECK(bias.numel() > 0, "non-empty bias tensor expected");
      const int64_t n_output_plane = bias.dim() == 0 ? 1 : bias.size(0);
      check_dim_size(grad_output, ndim, dim_planes, n_output_plane);
    }
    check_dim_size(grad_output, ndim, dim_depth, output_depth);
    check_dim_size(grad_output, ndim, dim_height, output_height);
    check_dim_size(grad_output, ndim, dim_width, output_width);
  }
}

static Tensor view_weight_2d(const Tensor& weight_) {
  Tensor weight = weight_.contiguous();
  if (weight.dim() == 5) {
    const int64_t s1 = weight.size(0);
    const int64_t s2 =
        weight.size(1) * weight.size(2) * weight.size(3) * weight.size(4);
    return weight.view({s1, s2});
  } else {
    return weight;
  }
}

static void slow_conv3d_update_output_frame(
    Tensor& input,
    Tensor& output,
    const Tensor& weight,
    const Tensor& bias,
    Tensor& finput,
    int64_t kernel_depth,
    int64_t kernel_height,
    int64_t kernel_width,
    int64_t stride_depth,
    int64_t stride_height,
    int64_t stride_width,
    int64_t pad_depth,
    int64_t pad_height,
    int64_t pad_width,
    int64_t n_input_plane,
    int64_t input_depth,
    int64_t input_height,
    int64_t input_width,
    int64_t n_output_plane,
    int64_t output_depth,
    int64_t output_height,
    int64_t output_width) {
  Unfold3dCopyCPU(
      input,
      n_input_plane,
      input_depth,
      input_height,
      input_width,
      output_depth,
      output_height,
      output_width,
      kernel_depth,
      kernel_height,
      kernel_width,
      stride_depth,
      stride_height,
      stride_width,
      pad_depth,
      pad_height,
      pad_width,
      &finput);
  auto output2d = output.reshape(
      {n_output_plane, output_depth * output_height * output_width});
  if (bias.defined()) {
    for (int64_t i = 0; i < n_output_plane; ++i) {
      output[i].fill_(bias[i].item());
    }
    output2d.addmm_(weight, finput, 1, 1);
  } else {
    at::mm_out(output2d, weight, finput);
  }
}

void slow_conv3d_backward_update_grad_input_frame(
    Tensor& grad_input,
    const Tensor& grad_output,
    const Tensor& weight,
    Tensor& fgrad_input,
    int64_t kernel_depth,
    int64_t kernel_height,
    int64_t kernel_width,
    int64_t stride_depth,
    int64_t stride_height,
    int64_t stride_width,
    int64_t pad_depth,
    int64_t pad_height,
    int64_t pad_width) {
  auto grad_output_2d = grad_output.reshape(
      {grad_output.size(0),
       grad_output.size(1) * grad_output.size(2) * grad_output.size(3)});
  fgrad_input.addmm_(weight, grad_output_2d, 0, 1);

  grad_input.zero_();
  unfolded3d_acc_kernel_cpu(
      fgrad_input,
      grad_input,
      kernel_depth,
      kernel_height,
      kernel_width,
      stride_depth,
      stride_height,
      stride_width,
      pad_depth,
      pad_height,
      pad_width,
      grad_input.size(0),
      grad_input.size(1),
      grad_input.size(2),
      grad_input.size(3),
      grad_output.size(1),
      grad_output.size(2),
      grad_output.size(3));
}

void slow_conv3d_backward_out_cpu_template(
    Tensor& grad_input,
    const Tensor& grad_output,
    const Tensor& input,
    const Tensor& weight,
    const Tensor& finput,
    Tensor& fgrad_input,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding) {
  const int64_t kernel_depth = kernel_size[0];
  const int64_t kernel_height = kernel_size[1];
  const int64_t kernel_width = kernel_size[2];
  const int64_t pad_depth = padding[0];
  const int64_t pad_height = padding[1];
  const int64_t pad_width = padding[2];
  const int64_t stride_depth = stride[0];
  const int64_t stride_height = stride[1];
  const int64_t stride_width = stride[2];

  slow_conv3d_shape_check(
      input,
      grad_output,
      weight,
      Tensor(),
      kernel_depth,
      kernel_height,
      kernel_width,
      stride_depth,
      stride_height,
      stride_width,
      pad_depth,
      pad_height,
      pad_width,
      false);

  const Tensor weight2d = view_weight_2d(weight);
  const Tensor grad_output_contiguous = grad_output.contiguous();
  grad_input.resize_as_(input);
  TORCH_CHECK(grad_input.is_contiguous(), "grad_input must be contiguous")
  fgrad_input.resize_as_(finput);
  TORCH_CHECK(fgrad_input.is_contiguous(), "fgrad_input must be contiguous")
  fgrad_input.zero_();
  const Tensor tweight2d = weight2d.transpose(0, 1);
  const int64_t batch_size = input.size(0);
  at::parallel_for(
      0, batch_size, CONV3D_GRAIN_SALT, [&](int64_t start, int64_t end) {
        AutoNonVariableTypeMode non_variable_type_mode;
        for (int64_t t = start; t < end; t++) {
          Tensor grad_input_t = grad_input[t];
          Tensor grad_output_t = grad_output_contiguous[t];
          Tensor fgrad_input_t = fgrad_input[t];
          slow_conv3d_backward_update_grad_input_frame(
              grad_input_t,
              grad_output_t,
              tweight2d,
              fgrad_input_t,
              kernel_depth,
              kernel_height,
              kernel_width,
              stride_depth,
              stride_height,
              stride_width,
              pad_depth,
              pad_height,
              pad_width);
        }
      });
}

void slow_conv3d_backward_parameters_frame(
    Tensor& grad_weight,
    Tensor& grad_bias,
    Tensor& grad_output,
    const Tensor& finput) {
  auto grad_output_2d = grad_output.view(
      {grad_output.size(0),
       grad_output.size(1) * grad_output.size(2) * grad_output.size(3)});

  if (grad_weight.defined()) {
    const Tensor tfinput = finput.transpose(0, 1);
    grad_weight.addmm_(grad_output_2d, tfinput);
  }

  if (grad_bias.defined()) {
    AT_DISPATCH_FLOATING_TYPES_AND(
        at::ScalarType::BFloat16,
        grad_output.scalar_type(),
        "slow_conv3d_backward_parameters",
        [&] {
          auto grad_output_2d_acc = grad_output_2d.accessor<scalar_t, 2>();
          auto grad_bias_acc = grad_bias.accessor<scalar_t, 1>();
          const auto sz = grad_output_2d.size(1);
          for (int64_t i = 0; i < grad_bias.size(0); i++) {
            scalar_t sum = 0;
            for (int64_t k = 0; k < sz; k++) {
              sum += grad_output_2d_acc[i][k];
            }
            grad_bias_acc[i] += sum;
          }
        });
  }
}

static void slow_conv3d_backward_parameters_out_cpu_template(
    Tensor& grad_weight,
    Tensor& grad_bias,
    const Tensor& input,
    const Tensor& grad_output,
    const Tensor& finput,
    Tensor fgrad_input,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding) {
  CheckedFrom c = "slow_conv3d_backward_parameters_cpu";
  auto grad_weight_arg = TensorArg(grad_weight, "grad_weight_arg", 0);
  auto grad_bias_arg = TensorArg(grad_bias, "grad_bias_arg", 0);

  const int64_t kernel_depth = kernel_size[0];
  const int64_t kernel_height = kernel_size[1];
  const int64_t kernel_width = kernel_size[2];
  const int64_t pad_depth = padding[0];
  const int64_t pad_height = padding[1];
  const int64_t pad_width = padding[2];
  const int64_t stride_depth = stride[0];
  const int64_t stride_height = stride[1];
  const int64_t stride_width = stride[2];

  slow_conv3d_shape_check(
      input,
      grad_output,
      grad_weight,
      grad_bias,
      kernel_depth,
      kernel_height,
      kernel_width,
      stride_depth,
      stride_height,
      stride_width,
      pad_depth,
      pad_height,
      pad_width,
      true);

  Tensor grad_weight_2d;
  if (grad_weight.defined()) {
    checkContiguous(c, grad_weight_arg);
    grad_weight_2d = view_weight_2d(grad_weight);
  }

  if (grad_bias.defined()) {
    checkContiguous(c, grad_bias_arg);
  }

  auto grad_output_contiguous = grad_output.contiguous();

  const int64_t batch_size = input.size(0);
  at::parallel_for(
      0, batch_size, CONV3D_GRAIN_SALT, [&](int64_t start, int64_t end) {
        for (int64_t t = start; t < end; t++) {
          Tensor grad_output_t = grad_output_contiguous[t];
          Tensor finput_t;
          if (grad_weight_2d.defined()) {
            finput_t = finput[t];
          }

          slow_conv3d_backward_parameters_frame(
              grad_weight_2d, grad_bias, grad_output_t, finput_t);
        }
      });
}

} // namespace

std::tuple<Tensor&, Tensor&, Tensor&> slow_conv3d_forward_out_cpu(
    Tensor& output,
    Tensor& finput,
    Tensor& fgrad_input,
    const Tensor& self,
    const Tensor& weight,
    IntArrayRef kernel_size,
    const Tensor& bias,
    IntArrayRef stride,
    IntArrayRef padding) {
  const int64_t kernel_depth = kernel_size[0];
  const int64_t kernel_height = kernel_size[1];
  const int64_t kernel_width = kernel_size[2];
  const int64_t pad_depth = padding[0];
  const int64_t pad_height = padding[1];
  const int64_t pad_width = padding[2];
  const int64_t stride_depth = stride[0];
  const int64_t stride_height = stride[1];
  const int64_t stride_width = stride[2];

  slow_conv3d_shape_check(
      self,
      Tensor(),
      weight,
      bias,
      kernel_depth,
      kernel_height,
      kernel_width,
      stride_depth,
      stride_height,
      stride_width,
      pad_depth,
      pad_height,
      pad_width,
      false);

  const Tensor input = self.contiguous();
  const Tensor weight_2d = view_weight_2d(weight);

  const int64_t ndim = input.dim();
  const int64_t dim_planes = 1;
  const int64_t dim_depth = 2;
  const int64_t dim_height = 3;
  const int64_t dim_width = 4;

  const int64_t n_input_plane = input.size(dim_planes);
  const int64_t input_depth = input.size(dim_depth);
  const int64_t input_height = input.size(dim_height);
  const int64_t input_width = input.size(dim_width);
  const int64_t n_output_plane = weight_2d.size(0);
  const int64_t output_depth =
      (input_depth + 2 * pad_depth - kernel_depth) / stride_depth + 1;
  const int64_t output_height =
      (input_height + 2 * pad_height - kernel_height) / stride_height + 1;
  const int64_t output_width =
      (input_width + 2 * pad_width - kernel_width) / stride_width + 1;

  const int64_t batch_size = input.size(0);
  finput.resize_({batch_size,
                  n_input_plane * kernel_depth * kernel_height * kernel_width,
                  output_depth * output_height * output_width});
  output.resize_(
      {batch_size, n_output_plane, output_depth, output_height, output_width});

  at::parallel_for(
      0, batch_size, CONV3D_GRAIN_SALT, [&](int64_t start, int64_t end) {
        AutoNonVariableTypeMode non_variable_type_mode;
        for (int64_t t = start; t < end; t++) {
          Tensor input_t = input[t];
          Tensor output_t = output[t];
          Tensor finput_t = finput[t];
          slow_conv3d_update_output_frame(
              input_t,
              output_t,
              weight_2d,
              bias,
              finput_t,
              kernel_depth,
              kernel_height,
              kernel_width,
              stride_depth,
              stride_height,
              stride_width,
              pad_depth,
              pad_height,
              pad_width,
              n_input_plane,
              input_depth,
              input_height,
              input_width,
              n_output_plane,
              output_depth,
              output_height,
              output_width);
        }
      });

  return std::tuple<Tensor&, Tensor&, Tensor&>(output, finput, fgrad_input);
}

std::tuple<Tensor, Tensor, Tensor> slow_conv3d_forward_cpu(
    const Tensor& self,
    const Tensor& weight,
    IntArrayRef kernel_size,
    const Tensor& bias,
    IntArrayRef stride,
    IntArrayRef padding) {
  auto output = at::empty({0}, self.options());
  auto finput = at::empty({0}, self.options());
  auto fgrad_input = at::empty({0}, self.options());
  slow_conv3d_forward_out_cpu(
      output,
      finput,
      fgrad_input,
      self,
      weight,
      kernel_size,
      bias,
      stride,
      padding);
  return std::make_tuple(output, finput, fgrad_input);
}

std::tuple<Tensor&, Tensor&, Tensor&> slow_conv3d_backward_out_cpu(
    Tensor& grad_input,
    Tensor& grad_weight,
    Tensor& grad_bias,
    const Tensor& grad_output,
    const Tensor& self,
    const Tensor& weight,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding,
    const Tensor& finput,
    const Tensor& fgrad_input) {
  if (grad_input.defined()) {
    slow_conv3d_backward_out_cpu_template(
        grad_input,
        grad_output,
        self,
        weight,
        finput,
        const_cast<Tensor&>(
            fgrad_input), // cast away auto-generated const of buffer
        kernel_size,
        stride,
        padding);
  }

  if (grad_weight.defined()) {
    grad_weight.resize_(weight.sizes());
    grad_weight.zero_();
  }

  if (grad_bias.defined()) {
    grad_bias.resize_({grad_output.size(1)});
    grad_bias.zero_();
  }

  if (grad_weight.defined() || grad_bias.defined()) {
    slow_conv3d_backward_parameters_out_cpu_template(
        grad_weight,
        grad_bias,
        self,
        grad_output,
        finput,
        fgrad_input,
        kernel_size,
        stride,
        padding);
  }

  return std::tuple<Tensor&, Tensor&, Tensor&>(
      grad_input, grad_weight, grad_bias);
}

std::tuple<Tensor, Tensor, Tensor> slow_conv3d_backward_cpu(
    const Tensor& grad_output,
    const Tensor& self,
    const Tensor& weight,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding,
    const Tensor& finput,
    const Tensor& fgrad_input,
    std::array<bool, 3> output_mask) {
  Tensor grad_input;
  Tensor grad_weight;
  Tensor grad_bias;

  if (output_mask[0]) {
    grad_input = at::empty({0}, grad_output.options());
  }

  if (output_mask[1]) {
    grad_weight = at::empty({0}, grad_output.options());
  }

  if (output_mask[2]) {
    grad_bias = at::empty({0}, grad_output.options());
  }

  slow_conv3d_backward_out_cpu(
      grad_input,
      grad_weight,
      grad_bias,
      grad_output,
      self,
      weight,
      kernel_size,
      stride,
      padding,
      finput,
      fgrad_input);

  return std::make_tuple(grad_input, grad_weight, grad_bias);
}

Tensor& slow_conv3d_out(
    Tensor& output,
    const Tensor& self,
    const Tensor& weight,
    IntArrayRef kernel_size,
    const Tensor& bias,
    IntArrayRef stride,
    IntArrayRef padding) {
  Tensor finput = at::empty({0}, self.options());
  Tensor fgrad_input = at::empty({0}, self.options());
  return std::get<0>(at::slow_conv3d_forward_out(
      output,
      finput,
      fgrad_input,
      self,
      weight,
      kernel_size,
      bias,
      stride,
      padding));
}

Tensor slow_conv3d(
    const Tensor& self,
    const Tensor& weight,
    IntArrayRef kernel_size,
    const Tensor& bias,
    IntArrayRef stride,
    IntArrayRef padding) {
  return std::get<0>(at::slow_conv3d_forward(
      self, weight, kernel_size, bias, stride, padding));
}

} // namespace native
} // namespace at
