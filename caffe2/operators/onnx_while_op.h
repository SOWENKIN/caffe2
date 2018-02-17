/**
 * Copyright (c) 2016-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CAFFE2_OPERATORS_ONNX_WHILE_OP_H_
#define CAFFE2_OPERATORS_ONNX_WHILE_OP_H_

#include "caffe2/core/context.h"
#include "caffe2/core/logging.h"
#include "caffe2/core/operator.h"

namespace caffe2 {

template <class Context>
class ONNXWhileOp final : public Operator<Context> {
 public:
  ONNXWhileOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator<Context>(operator_def, ws),
        ws_(ws),
        has_trip_count_(
            OperatorBase::GetSingleArgument<int64_t>("has_trip_count", 0)),
        has_cond_(OperatorBase::GetSingleArgument<int64_t>("has_cond", 0)) {
    CAFFE_ENFORCE(
        this->template HasSingleArgumentOfType<NetDef>("body"),
        "body net must be specified in ONNXWhile operator");
    body_net_def_ = this->template GetSingleArgument<NetDef>("body", NetDef());

    // Create loop-carried deps in Workspace
    for (int i = 2; i < body_net_def_.external_input_size(); ++i) {
      Blob* b = ws_->CreateBlob(body_net_def_.external_input(i));
      Tensor<Context>* t = b->template GetMutable<Tensor<Context>>();
      lcd_tensors_.push_back(t);
    }
    // First output is the iteration variable
    auto* iteration_var_blob = ws_->CreateBlob(body_net_def_.external_input(0));
    iteration_var_ = iteration_var_blob->template GetMutable<Tensor<Context>>();

    input_condition_var_ = ws_->CreateBlob(body_net_def_.external_input(1))
                               ->template GetMutable<Tensor<Context>>();

    auto* condition_var_blob =
        ws_->CreateBlob(body_net_def_.external_output(0));
    condition_var_ = condition_var_blob->template GetMutable<Tensor<Context>>();

    body_net_ = CreateNet(body_net_def_, ws);
    CAFFE_ENFORCE(body_net_, "Failed to initialize loop subnet");
  }

  USE_OPERATOR_CONTEXT_FUNCTIONS;

  // Operator
  //  Inputs: max trip count, condition, initial loop-carried dependencies
  //  Outputs: Final loop-carried dependencies, scan_outputs
  // Body
  //  Inputs: iteration number, condition, loop-carried dependencies
  //  Outputs: condition, loop-carried dependencies, scan_outputs
  bool RunOnDevice() override {
    constexpr int64_t num_inputs_before_lcds = 2;
    // First input is the maximumt trip count. Second input is the condition
    // variable (for the first iteration). The rest of the inputs are
    // loop-carried dependencies.
    int num_loop_carried_deps = InputSize() - num_inputs_before_lcds;
    int64_t max_trip_count = *Input(0).template data<int64_t>();
    const bool first_iter_condition = *Input(1).template data<bool>();

    // Body graph has 2+N inputs: iteration number, condition value, and N
    // loop-carried dependencies
    CAFFE_ENFORCE_EQ(
        num_loop_carried_deps + 2,
        body_net_->external_input().size(),
        "Body graph must have 2+N inputs, where N is the number of "
        "loop carried dependencies.");

    // Body graph has 1+N+K outputs: recalculated condition variable, N
    // loop-carried dependencies, and K scan_outputs
    int num_scan_outputs =
        body_net_->external_output().size() - num_loop_carried_deps - 1;

    CAFFE_ENFORCE_GE(
        num_scan_outputs,
        0,
        "Body graph must have N+K outputs, where N is the number "
        "of loop-carried dependencies and K is the number of scan "
        "outputs");

    // Copy initial loop-carried dependencies
    for (int i = 0; i < num_loop_carried_deps; ++i) {
      lcd_tensors_[i]->CopyFrom(Input(i + num_inputs_before_lcds));
    }

    // Initialize iteration variable
    iteration_var_->Resize(1);
    auto* iteration_var_ptr = iteration_var_->template mutable_data<int64_t>();
    *iteration_var_ptr = 0ll;

    // Input condition var. This requires special handling
    input_condition_var_->Resize(1);
    auto* input_condition_var_ptr =
        input_condition_var_->template mutable_data<bool>();
    *input_condition_var_ptr = first_iter_condition;

    // Output condition var. This is yielded by the body net and we will use its
    // value to determine further iteration

    condition_var_->Resize(1);
    auto* condition_var_ptr = condition_var_->template mutable_data<bool>();

    auto valid_iter_num = [this, max_trip_count](int64_t i) {
      if (has_trip_count_) {
        return i < max_trip_count;
      } else {
        return true;
      }
    };

    auto condition_true =
        [this, first_iter_condition, condition_var_ptr](int64_t i) {
          if (has_cond_) {
            if (i == 0) {
              return (bool)first_iter_condition;
            } else {
              return (bool)*condition_var_ptr;
            }
          } else {
            return true;
          }
        };

    // Allocate scan_outputs for zero-iteration case
    for (int i = 0; i < num_scan_outputs; ++i) {
      Output(i + num_loop_carried_deps)->Resize(0);
      Output(i + num_loop_carried_deps)->template mutable_data<int32_t>();
    }

    // Use this to keep track of the sizes of the scan outputs and validate
    // they're the same across iterations.
    std::vector<std::vector<TIndex>> scan_outputs_sizes;

    while (true) {
      int64_t itr = *iteration_var_ptr;
      if (valid_iter_num(itr) && condition_true(itr)) {
        if (!body_net_->Run()) {
          return false;
        }
        // Copy forward loop-carried dependencies
        for (int i = 0; i < num_loop_carried_deps; ++i) {
          Blob* b = ws_->GetBlob(body_net_->external_output()[i + 1]);
          const Tensor<Context>& t = b->template Get<Tensor<Context>>();
          lcd_tensors_[i]->CopyFrom(t);
        }
        // Copy out scan_outputs
        for (int i = 0; i < num_scan_outputs; ++i) {
          int net_output_idx = i + 1 + num_loop_carried_deps;
          const Tensor<Context>& scan_output =
              ws_->GetBlob(body_net_->external_output()[net_output_idx])
                  ->template Get<Tensor<Context>>();
          auto* scan_output_target = Output(i + num_loop_carried_deps);
          if (itr == 0) {
            auto dims = scan_output.dims();
            scan_outputs_sizes.push_back(dims);
            dims.insert(dims.begin(), 1);
            scan_output_target->Resize(dims);
            scan_output_target->CopyFrom(scan_output);
          } else {
            auto dims = scan_output.dims();
            CAFFE_ENFORCE_EQ(
                dims,
                scan_outputs_sizes[i],
                "Size of scan output changed across iterations");
            dims.insert(dims.begin(), itr);
            scan_output_target->Extend(1, 2.0f, &context_);

            TIndex timestep_size = 1;
            for (const TIndex t : scan_outputs_sizes[i]) {
              timestep_size *= t;
            }

            const void* src_data = scan_output.raw_data();
            auto& sot_meta = scan_output_target->meta();
            void* dst_data =
                (char*)scan_output_target->raw_mutable_data(sot_meta) +
                timestep_size * scan_output.itemsize() * itr;
            memcpy(dst_data, src_data, timestep_size * scan_output.itemsize());
          }
        }
      } else {
        break;
      }
      *iteration_var_ptr += 1ll;
      *input_condition_var_ptr = *condition_var_ptr;
    }

    if (*iteration_var_ptr > 0) {
      // Copy out final loop-carried dependencies
      for (int i = 0; i < num_loop_carried_deps; ++i) {
        Output(i)->CopyFrom(*lcd_tensors_[i]);
      }
    } else {
      // Copy out final loop-carried dependencies
      for (int i = 0; i < num_loop_carried_deps; ++i) {
        Output(i)->CopyFrom(Input(i + num_inputs_before_lcds));
      }
    }

    return true;
  }

  NetDef body_net_def_;
  std::unique_ptr<NetBase> body_net_;
  Workspace* ws_;

  bool has_trip_count_, has_cond_;

  Tensor<Context>*iteration_var_, *input_condition_var_, *condition_var_;

  std::vector<Tensor<Context>*> lcd_tensors_;
};

} // namespace caffe2

#endif // CAFFE2_OPERATORS_ONNX_WHILE_OP_H