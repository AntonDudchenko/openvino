// Copyright (C) 2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "transformations/handle_transposes_around_matmul.hpp"

#include <numeric>

#include <openvino/cc/ngraph/itt.hpp>
#include <ngraph/rt_info.hpp>
#include <ngraph/opsets/opset8.hpp>
#include <ngraph/pattern/op/or.hpp>
#include <ngraph/pattern/op/wrap_type.hpp>
#include <ie/ie_common.h>

#include "gna_plugin_log.hpp"
#include "backend/gna_limitations.hpp"

namespace GNAPluginNS {

NGRAPH_RTTI_DEFINITION(HandleTransposesAroundMatMul, "HandleTransposesAroundMatMul", 0);
NGRAPH_RTTI_DEFINITION(HandleTransposeBeforeMatMul, "HandleTransposeBeforeMatMul", 0);
NGRAPH_RTTI_DEFINITION(HandleTransposeAfterMatMul, "HandleTransposeAfterMatMul", 0);

void ReplaceTransposeWithReshape(std::shared_ptr<ngraph::Node> transpose_node) {
    auto shape = transpose_node->get_output_shape(0);
    auto reshape_const = std::make_shared<ngraph::opset8::Constant>(ngraph::element::Type_t::i64,
        ngraph::Shape{shape.size()}, shape);
    auto reshape_node = std::make_shared<ngraph::opset8::Reshape>(transpose_node->input_value(0), reshape_const, false);
    reshape_node->set_friendly_name(transpose_node->get_friendly_name());
    ngraph::copy_runtime_info(transpose_node, reshape_node);
    transpose_node->output(0).replace(reshape_node->output(0));
}

void InsertTranspose(std::shared_ptr<ngraph::Node> prev_node, const std::string& base_name) {
    auto consumers = prev_node->output(0).get_target_inputs();
    const auto orig_shape = prev_node->get_output_shape(0);
    std::vector<size_t> transpose_ids;
    for (size_t i = 0; i < orig_shape.size(); ++i) {
        if (orig_shape[i] > 1) {
            transpose_ids.push_back(i);
        }
    }
    IE_ASSERT(transpose_ids.size() == 2);
    std::vector<size_t> permute_order(orig_shape.size());
    std::iota(std::begin(permute_order), std::end(permute_order), 0);
    std::swap(permute_order[transpose_ids[0]], permute_order[transpose_ids[1]]);

    auto transpose_order = ngraph::opset8::Constant::create(ngraph::element::i64, ngraph::Shape{permute_order.size()}, permute_order);
    auto transpose = std::make_shared<ngraph::opset8::Transpose>(prev_node, transpose_order);
    transpose->set_friendly_name(base_name + "/in_transpose");

    auto reshapeConstAfter = std::make_shared<ngraph::opset8::Constant>(ngraph::element::Type_t::i64,
        ngraph::Shape{orig_shape.size()}, orig_shape);
    auto reshapeAfter = std::make_shared<ngraph::opset8::Reshape>(transpose, reshapeConstAfter, false);
    reshapeAfter->set_friendly_name(base_name + "/reshape_after_transpose");
    ngraph::copy_runtime_info(prev_node, ngraph::NodeVector{transpose, reshapeAfter});

    for (auto input : consumers) {
        input.replace_source_output(reshapeAfter);
    }
}

static bool VerifyReshape(const ngraph::Output<ngraph::Node>& reshape_out) {
    auto in_shape = reshape_out.get_node_shared_ptr()->get_input_shape(0);
    auto out_shape = reshape_out.get_node_shared_ptr()->get_output_shape(0);
    return in_shape[0] != out_shape[0];
}

HandleTransposeBeforeMatMul::HandleTransposeBeforeMatMul() {
    auto constant = ngraph::pattern::wrap_type<ngraph::opset8::Constant>();
    auto fq = ngraph::pattern::wrap_type<ngraph::opset8::FakeQuantize>({constant, ngraph::pattern::any_input(),
        ngraph::pattern::any_input(), ngraph::pattern::any_input(), ngraph::pattern::any_input()});
    auto reshape = ngraph::pattern::wrap_type<ngraph::opset8::Reshape>({}, VerifyReshape);
    auto transpose = ngraph::pattern::wrap_type<ngraph::opset8::Transpose>({reshape,
        ngraph::pattern::any_input()});
    auto matmul1 = ngraph::pattern::wrap_type<ngraph::opset8::MatMul>({
        std::make_shared<ngraph::pattern::op::Or>(ngraph::OutputVector{reshape, transpose}),
        ngraph::pattern::any_input()});
    auto matmul2 = ngraph::pattern::wrap_type<ngraph::opset8::MatMul>({
        std::make_shared<ngraph::pattern::op::Or>(ngraph::OutputVector{constant, fq}),
        std::make_shared<ngraph::pattern::op::Or>(ngraph::OutputVector{reshape, transpose, ngraph::pattern::any_input()})});
    auto matmul = std::make_shared<ngraph::pattern::op::Or>(ngraph::OutputVector{matmul1, matmul2});

    ngraph::matcher_pass_callback callback = [=](ngraph::pattern::Matcher &matcher) {
        const auto& pattern_map = matcher.get_pattern_value_map();
        auto matmul_iter = pattern_map.find(matmul1);
        if (matmul_iter == std::end(pattern_map) &&
            (matmul_iter = pattern_map.find(matmul2)) == std::end(pattern_map)) {
            return false;
        }

        auto transpose_reshape_it = pattern_map.find(transpose);
        if (transpose_reshape_it != std::end(pattern_map)) {
            ReplaceTransposeWithReshape(transpose_reshape_it->second.get_node_shared_ptr());
        } else if ((transpose_reshape_it = pattern_map.find(reshape)) != std::end(pattern_map)) {
            auto reshape_node = pattern_map.at(reshape).get_node_shared_ptr();
            if (GNALimitations::IsTransposeSupported(reshape_node->get_output_shape(0))) {
                auto matmul_node = matmul_iter->second.get_node_shared_ptr();
                InsertTranspose(reshape_node, matmul_node->get_friendly_name());
            }
        }

        auto iter = pattern_map.find(fq);
        if (iter != pattern_map.end() ||
            (iter = pattern_map.find(constant)) != pattern_map.end()) {
            auto prev_node = iter->second.get_node_shared_ptr();
            if (!GNALimitations::IsTransposeSupported(prev_node->get_output_shape(0))) return false;
            auto matmul_node = iter->second.get_node_shared_ptr();
            InsertTranspose(prev_node, matmul_node->get_friendly_name());
        }
        return true;
    };

    auto matcher = std::make_shared<ngraph::pattern::Matcher>(matmul, "HandleTransposeBeforeMatMul");
    this->register_matcher(matcher, callback);
}

HandleTransposeAfterMatMul::HandleTransposeAfterMatMul() {
    auto matmul = ngraph::pattern::wrap_type<ngraph::opset8::MatMul>();
    auto add_left = ngraph::pattern::wrap_type<ngraph::opset8::Add>({matmul, ngraph::pattern::any_input()});
    auto add_right = ngraph::pattern::wrap_type<ngraph::opset8::Add>({ngraph::pattern::any_input(), matmul});
    auto fq_input = std::make_shared<ngraph::pattern::op::Or>(ngraph::OutputVector{matmul, add_left, add_right});
    auto fq = ngraph::pattern::wrap_type<ngraph::opset8::FakeQuantize>({fq_input, ngraph::pattern::any_input(),
        ngraph::pattern::any_input(), ngraph::pattern::any_input(), ngraph::pattern::any_input()});
    auto transpose_input = std::make_shared<ngraph::pattern::op::Or>(ngraph::OutputVector{fq_input, fq});
    auto transpose = ngraph::pattern::wrap_type<ngraph::opset8::Transpose>({transpose_input, ngraph::pattern::any_input()});
    auto reshape_input = std::make_shared<ngraph::pattern::op::Or>(ngraph::OutputVector{transpose_input, transpose});
    auto reshape = ngraph::pattern::wrap_type<ngraph::opset8::Reshape>(
        {reshape_input, ngraph::pattern::any_input()}, VerifyReshape);

    ngraph::matcher_pass_callback callback = [=](ngraph::pattern::Matcher &matcher) {
        const auto& pattern_map = matcher.get_pattern_value_map();
        auto transpose_it = pattern_map.find(transpose);
        if (transpose_it != std::end(pattern_map)) {
            ReplaceTransposeWithReshape(transpose_it->second.get_node_shared_ptr());
        } else {
            auto reshape_node = pattern_map.at(reshape).get_node_shared_ptr();
            if (!GNALimitations::IsTransposeSupported(reshape_node->get_output_shape(0))) return false;
            auto iter = pattern_map.find(fq);
            if (iter == pattern_map.end() &&
                (iter = pattern_map.find(add_left)) == pattern_map.end() &&
                (iter = pattern_map.find(add_right)) == pattern_map.end() &&
                (iter = pattern_map.find(matmul)) == pattern_map.end()) {
                return false;
            }
            auto node = iter->second.get_node_shared_ptr();
            InsertTranspose(node, node->get_friendly_name());
        }
        return true;
    };

    auto matcher = std::make_shared<ngraph::pattern::Matcher>(reshape, "HandleTransposeAfterMatMul");
    this->register_matcher(matcher, callback);
}

HandleTransposesAroundMatMul::HandleTransposesAroundMatMul() {
    add_matcher<HandleTransposeBeforeMatMul>();
    add_matcher<HandleTransposeAfterMatMul>();
}

} // namespace GNAPluginNS