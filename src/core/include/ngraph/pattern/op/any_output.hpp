// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include "ngraph/node.hpp"
#include "ngraph/pattern/op/pattern.hpp"
#include "openvino/pass/pattern/op/any_output.hpp"

namespace ngraph {
namespace pattern {
namespace op {
using ov::pass::pattern::op::AnyOutput;
}  // namespace op
}  // namespace pattern
}  // namespace ngraph