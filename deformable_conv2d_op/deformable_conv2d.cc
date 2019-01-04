#include "deformable_conv2d.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/util/tensor_format.h"
#include "tensorflow/core/util/padding.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/framework/common_shape_fns.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "deformable_conv2d_utils.h"
#include <cmath>

namespace tensorflow{
typedef std::vector<int32> TShape;

// the inputs should have format NCHW, which is faster on GPUs
// Once the attrs set, the values of them will not change any more during the training process.
REGISTER_OP("DeformableConv2D")
    .Input("input: T")
    .Input("filter: T")
    .Input("offset: T")
    .Input("mask: T")
    .Output("output: T")
    .Attr("T: {half, bfloat16, float, double}")
    .Attr("strides: list(int)")
    .Attr("use_cudnn_on_gpu: bool = true")
    .Attr("num_groups: int")
    .Attr("deformable_groups: int")
    .Attr("im2col_step: int")
    .Attr("no_bias: bool = true")
    .Attr(GetPaddingAttrString())
    .Attr(GetConvnetDataFormatAttrString())
    .Attr("dilations: list(int) = [1, 1, 1, 1]")
    .SetShapeFn([](::tensorflow::shape_inference::InferenceContext* c){
            
            string data_format_str, filter_format_str;
            if (!c->GetAttr("data_format", &data_format_str).ok()) {
                data_format_str = "NHWC";
            }
            if (!c->GetAttr("filter_format", &filter_format_str).ok()) {
                filter_format_str = "HWIO";
            }

            TensorFormat data_format;
            if (!FormatFromString(data_format_str, &data_format)) {
                return errors::InvalidArgument("Invalid data format string: ",
                                            data_format_str);
            }
            FilterTensorFormat filter_format;
            if (!FilterFormatFromString(filter_format_str, &filter_format)) {
                return errors::InvalidArgument("Invalid filter format string: ",
                                            filter_format_str);
            }

            constexpr int num_spatial_dims = 2;
            const int rank = GetTensorDimsFromSpatialDims(num_spatial_dims, data_format);
            ::tensorflow::shape_inference::ShapeHandle conv_input_shape;
            TF_RETURN_IF_ERROR(c->WithRank(c->input(0), rank, &conv_input_shape));
            TF_RETURN_IF_ERROR(CheckFormatConstraintsOnShape(
                data_format, conv_input_shape, "conv_input", c));

            // The filter rank should match the input (4 for NCHW, 5 for NCHW_VECT_C).
            ::tensorflow::shape_inference::ShapeHandle filter_shape;
            TF_RETURN_IF_ERROR(c->WithRank(c->input(1), rank, &filter_shape));
            TF_RETURN_IF_ERROR(
                CheckFormatConstraintsOnShape(data_format, filter_shape, "filter", c));

            std::vector<int32> dilations;
            TF_RETURN_IF_ERROR(c->GetAttr("dilations", &dilations));

            if (dilations.size() != 4) {
                return errors::InvalidArgument(
                    "Conv2D requires the dilation attribute to contain 4 values, but got: ",
                    dilations.size());
            }

            std::vector<int32> strides;
            TF_RETURN_IF_ERROR(c->GetAttr("strides", &strides));

            // strides.size() should be 4 (NCHW) even if the input is 5 (NCHW_VECT_C).
            if (strides.size() != 4) {
                return errors::InvalidArgument("Conv2D on data format ", data_format_str,
                                            " requires the stride attribute to contain"
                                            " 4 values, but got: ",
                                            strides.size());
            }

            const int32 stride_rows = GetTensorDim(strides, data_format, 'H');
            const int32 stride_cols = GetTensorDim(strides, data_format, 'W');
            const int32 dilation_rows = GetTensorDim(dilations, data_format, 'H');
            const int32 dilation_cols = GetTensorDim(dilations, data_format, 'W');

            ::tensorflow::shape_inference::DimensionHandle batch_size_dim;
            ::tensorflow::shape_inference::DimensionHandle input_depth_dim;
            gtl::InlinedVector<::tensorflow::shape_inference::DimensionHandle, 2> input_spatial_dims(2);
            TF_RETURN_IF_ERROR(DimensionsFromShape(
                conv_input_shape, data_format, &batch_size_dim,
                absl::MakeSpan(input_spatial_dims), &input_depth_dim, c));

            ::tensorflow::shape_inference::DimensionHandle output_depth_dim = c->Dim(
                filter_shape, GetFilterDimIndex<num_spatial_dims>(filter_format, 'O'));
            ::tensorflow::shape_inference::DimensionHandle filter_rows_dim = c->Dim(
                filter_shape, GetFilterDimIndex<num_spatial_dims>(filter_format, 'H'));
            ::tensorflow::shape_inference::DimensionHandle filter_cols_dim = c->Dim(
                filter_shape, GetFilterDimIndex<num_spatial_dims>(filter_format, 'W'));
            ::tensorflow::shape_inference::DimensionHandle filter_input_depth_dim;
            if (filter_format == FORMAT_OIHW_VECT_I) {
                TF_RETURN_IF_ERROR(c->Multiply(
                    c->Dim(filter_shape,
                        GetFilterDimIndex<num_spatial_dims>(filter_format, 'I')),
                    c->Dim(filter_shape,
                        GetFilterTensorInnerInputChannelsDimIndex(rank, filter_format)),
                    &filter_input_depth_dim));
            } else {
                filter_input_depth_dim = c->Dim(
                    filter_shape, GetFilterDimIndex<num_spatial_dims>(filter_format, 'I'));
            }

            // Check that the input tensor and the filter tensor agree on the input
            // channel count.
            ::tensorflow::shape_inference::DimensionHandle unused;
            TF_RETURN_IF_ERROR(
                c->Merge(input_depth_dim, filter_input_depth_dim, &unused));

            Padding padding;
            TF_RETURN_IF_ERROR(c->GetAttr("padding", &padding));

            ::tensorflow::shape_inference::DimensionHandle output_rows, output_cols;
            TF_RETURN_IF_ERROR(GetWindowedOutputSizeFromDimsV2(
                c, input_spatial_dims[0], filter_rows_dim, dilation_rows, stride_rows,
                padding, &output_rows));
            TF_RETURN_IF_ERROR(GetWindowedOutputSizeFromDimsV2(
                c, input_spatial_dims[1], filter_cols_dim, dilation_cols, stride_cols,
                padding, &output_cols));

            ::tensorflow::shape_inference::ShapeHandle output_shape;
            TF_RETURN_IF_ERROR(
                ShapeFromDimensions(batch_size_dim, {output_rows, output_cols},
                                    output_depth_dim, data_format, c, &output_shape));
            c->set_output(0, output_shape);


            // can use following function to make more check on input shape
            // use WithValue check DimensionHandle, and use WithRank check ShapeHandle
            shape_inference::ShapeHandle offset_shape = c->input(2);
            shape_inference::ShapeHandle mask_shape = c->input(3);
            shape_inference::DimensionHandle offset_batch = c->Dim(offset_shape, 0);
            shape_inference::DimensionHandle offset_channel = c->Dim(offset_shape, 1);
            shape_inference::DimensionHandle offset_height = c->Dim(offset_shape, 2);
            shape_inference::DimensionHandle offset_weight = c->Dim(offset_shape, 3);
            shape_inference::DimensionHandle mask_channel = c->Dim(mask_shape, 1);
            shape_inference::DimensionHandle mask_height = c->Dim(mask_shape, 2);
            shape_inference::DimensionHandle mask_weight = c->Dim(mask_shape, 3);
            shape_inference::DimensionHandle mask_batch = c->Dim(mask_shape, 0);
            TF_RETURN_IF_ERROR(c->WithRank(offset_shape, 4, &offset_shape));
            TF_RETURN_IF_ERROR(c->WithRank(mask_shape, 4, &mask_shape));
            TF_RETURN_IF_ERROR(c->WithValue(offset_batch, c->Value(batch_size_dim), &offset_batch));
            TF_RETURN_IF_ERROR(c->WithValue(offset_channel, 2 * c->Value(filter_rows_dim) * c->Value(filter_cols_dim), &offset_channel));
            TF_RETURN_IF_ERROR(c->WithValue(offset_height, c->Value(output_rows), &offset_height));
            TF_RETURN_IF_ERROR(c->WithValue(offset_weight, c->Value(output_cols), &offset_weight));
            TF_RETURN_IF_ERROR(c->WithValue(mask_batch, c->Value(batch_size_dim), &mask_batch));
            TF_RETURN_IF_ERROR(c->WithValue(mask_channel, c->Value(filter_rows_dim) * c->Value(filter_cols_dim), &mask_channel));
            TF_RETURN_IF_ERROR(c->WithValue(mask_height, c->Value(output_rows), &mask_height));
            TF_RETURN_IF_ERROR(c->WithValue(mask_weight, c->Value(output_cols), &mask_weight));
            return Status::OK();
    })
    .Doc(R"doc(
        DeformableConv2D is a new convolution operation with the deformable kernel locations.
        The inputs should have format NCHW, which is faster on GPUS.
        The offset and mask should have same input spatial resolution.
        Also, the output's shape depends on the stride, and I only consider the situation of dilation rate = 1.
    )doc");

// Opkernel defination.
// template parameter <T> is the datatype of the tensors
// in my opnion, the deformable convolution op ought to be implemented by extending the Conv2DOp, however, we can not get the conv_ops.h file if we choose to dynamic link the op
template<typename Device, typename T>
class DeformableConv2DOp : public OpKernel{
    public:
    explicit DeformableConv2DOp(OpKernelConstruction* context) : OpKernel(context){
        // init the original parameters to the traditional paramaters.
        // the macro OP_REQUIRES_OK is used to judge whether or not the init operation were done successfully 
        OP_REQUIRES_OK(context, InitDeformableConv2DParameters(context. &params_));
        OP_REQUIRES_OK(context, context->GetAttr("use_cudnn_on_gpu", &use_cudnn_));
        use_cudnn_ &= CanUseCudnn();
        cudnn_use_autotune_ = CudnnUseAutotune();

        // the additional paramters of the deformable convolution were initiated below

    }

    void Compute(OpKernelContext* context) override{
        // Input tensor's shape
        // [batch, channels, height, weight]
        const Tensor& input = context->input(0);
        const TensorShape& input_shape = input.shape();
        // [filter_height, filter_weight, in_channels, out_channels]
        const Tensor& filter = context->input(1);
        const TensorShape& filter_shape = filter.shape();
        // [batch, 2 * filter.Size(), out_height, out_weight]
        const Tensor& offset = context->input(2);
        const TensorShape& offset_shape = offset.shape();
        // [batch, 2 * filter.Size(), out_height, out_weight]
        const Tensor& mask = context->input(3);
        const TensorShape& mask_shape = mask.shape();

        DeformableConv2DDimensions dimensions;
        OP_REQUIRES_OK(context, ComputeDeformableConv2DDimension(params_, input, filter, &dimensions));
        TensorShape out_shape = ShapeFromFormat(
        params_.data_format, dimensions.batch, dimensions.out_rows,
        dimensions.out_cols, dimensions.out_depth);

        // Output tensor is of the following dimensions:
        // [ in_batch, out_depth, out_rows, out_cols]
        Tensor* output = nullptr;
        OP_REQUIRES_OK(context, context->allocate_output(0, out_shape, &output));
        VLOG(2) << "DeformableConv2D: in_depth = " << dimensions.in_depth
            << ", patch_depth = " << dimensions.patch_depth
            << ", input_cols = " << dimensions.input_cols
            << ", filter_cols = " << dimensions.filter_cols
            << ", input_rows = " << dimensions.input_rows
            << ", filter_rows = " << dimensions.filter_rows
            << ", stride_rows = " << dimensions.stride_rows
            << ", stride_cols = " << dimensions.stride_cols
            << ", dilation_rows = " << dimensions.dilation_rows
            << ", dilation_cols = " << dimensions.dilation_cols
            << ", out_depth = " << dimensions.out_depth;

        // If there is nothing to compute, return.
        if (out_shape.num_elements() == 0) {
            return;
        }
        
        /**
         * from here i stop use the traditional convolution implement of the official code which was defined in conv_ops.cc
         * and began to use the implement of the deformable conv2d of the msra version
         * **/
        LayerSetUp(input_shape, filter_shape, offset_shape, mask_shape, out_shape);
        
        
    }

    private:
    DeformableConv2DParameters params_;
    bool use_cudnn_;
    bool cudnn_use_autotune_;
    index_t channel_axis_;  // channel axis of the input
    index_t channels_;  // number of channels of input image
    index_t num_spatial_axes_;  // number of spatial axes
    index_t num_;  // batch size
    index_t group_;  // number of groups
    index_t conv_out_channels_;  // number of output channels (num_filter)
    index_t conv_out_spatial_dim_;  // number of pixels of output images per channel
    index_t conv_in_channels_;  // number of input channels
    index_t kernel_dim_;  // number of input channels per group * kernel size
    index_t weight_offset_;  // number of output channels per group * kernel_dim_
    index_t col_offset_;
    index_t output_offset_;
    index_t col_buffer_size_;
    index_t input_dim_;
    index_t input_offset_dim_;
    index_t input_mask_dim_;
    index_t output_dim_;
    index_t num_kernels_im2col_;
    index_t num_kernels_col2im_;
    index_t im2col_step_;
    bool bias_term_;  // has bias term?
    bool is_1x1_;
    void LayerSetUp(const TensorShape& ishape, const TensorShape& filter_shape, const TensorShape& offset_shape, const TensorShape& mask_shape, const TensorShape& oshape) {
        channel_axis_ = 1;  // hard code channel axis, fixed the input data_format
        const index_t first_spatial_axis = channel_axis_ + 1;
        const index_t num_axes = filter_shape.dims(); //假设kernel的ndim = 2, 那么输入的ndim就是 4
        num_spatial_axes_ = num_axes - first_spatial_axis; //表示的是空间坐标个数,比如说2维卷积里,就是2, 3维卷积里就是3
        is_1x1_ = true; //  判断是否为1x1卷积
        for (index_t i = 0; i < filter_shape.dims() - 2; ++i) {
            // is_1x1_ &= filter_shape.dim_size(i) == 1 && params_.stride[i] == 1 && params_.pad[i] == 0;
            is_1x1_ &= filter_shape.dim_size(i) == 1; // only judge by the filter's shape
            if (!is_1x1_) break;
        }
        num_ = ishape.dim_size(0);// batch size
        channels_ = ishape.dim_size(1);// number of input channels
        group_ = params_.num_groups;//
        conv_out_channels_ = filter_shape.dim_size(3); // output channel nums
        conv_in_channels_ = channels_; // input channel nums
        bias_term_ = !params_.no_bias; //
        kernel_dim_ = conv_in_channels_ / group_ * filter_shape.dim_size(0) * filter_shape.dim_size(1); //Size()返回tensor中元素个数，即各维度大小的乘积，所以这里的kernel_dim的意思是卷积核的参数个数了．
        conv_out_spatial_dim_ = ProdShape(oshape, 2, oshape.dims()); //ProdShape(dimstart, dimend)返回指定维度大小乘积, 这个变量代表每个通道的像素点个数, oshape.ndim()返回这个shape的维度，假设是NCHW那么返回4,则为 H * W，
        col_offset_ = kernel_dim_ * conv_out_spatial_dim_;//kernel_dim代表一个卷积核参数的个数，conv_out_spatial_dim_相当于特征图上的坐标个数，那这个变量相当于总共需要的偏移量
        weight_offset_ = conv_out_channels_ * kernel_dim_ / group_;//这里应该是所有的权重的个数，也就是需要求的权重偏移的个数
        output_offset_ = conv_out_channels_ * conv_out_spatial_dim_ / group_;//这里是输出通道数乘上每个通道的像素点的个数，所以结果应该是输出的总维度，就是C*H*W
        im2col_step_ = std::min(params_.im2col_step, num_);
        col_buffer_size_ = kernel_dim_ * group_ * im2col_step_ * conv_out_spatial_dim_;// 开辟的缓存大小// size of the column buffer used for storing im2col-ed pixels
        input_dim_ = ProdShape(ishape, 1, ishape.dims());// input image size (#channels * height * width)
        input_offset_dim_ = ProdShape(offset_shape, 1, offset_shape.dims()); // 18 * H * W
        input_mask_dim_ = ProdShape(mask_shape, 1, mask_shape.dims()); // 9 * H * W
        output_dim_ = ProdShape(oshape, 1, oshape.dims()); //输出的元素个数
        num_kernels_im2col_ = conv_in_channels_ * conv_out_spatial_dim_; //如果输出和输入的分辨率不变的话，代表输入数据的dim,我个人觉得就是把整个输入展开为一个一维向量,在求其维度大小
        num_kernels_col2im_ = input_dim_;//输入数据的dim
  }


};

#define REGISTER_CPU(T)             \
    REGISTER_KERNEL_BUILDER(        \
        Name("DeformableConv2D").Device(DEVICE_CPU).TypeConstraint<T>("T"),\
        DeformableConv2DOp<CPUDevice, T>);
REGISTER_CPU(float);

#if GOOGLE_CUDA == 1
#define REGISTER_GPU(T)              \
    REGISTER_KERNEL_BUILDER(Name("DeformableConv2D".Device(DEVICE_GPU).TypeConstraint<T>("T")), DeformableConv2DOp<GPUDevice, T>); \
REGISTER_GPU(float);
#endif
}