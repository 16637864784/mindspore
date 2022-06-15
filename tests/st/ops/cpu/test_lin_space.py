# Copyright 2020 Huawei Technologies Co., Ltd
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ============================================================================

import numpy as np
import pytest

import mindspore.common.dtype as mstype
import mindspore.context as context
from mindspore import Tensor, ops
from mindspore.nn import Cell
from mindspore.ops import operations as P


class LinSpaceNet(Cell):
    def __init__(self, num):
        super(LinSpaceNet, self).__init__()
        self.ls_op = P.LinSpace()
        self.num = num

    def construct(self, start, stop):
        output = self.ls_op(start, stop, self.num)
        return output


@pytest.mark.level0
@pytest.mark.platform_x86_cpu
@pytest.mark.env_onecard
@pytest.mark.parametrize('start_np, stop_np', [(5, 150), (-25, 147), (-25.3, -147)])
@pytest.mark.parametrize('num_np', [12, 10, 20])
def test_lin_space(start_np, stop_np, num_np):
    """
    Feature: ALL To ALL
    Description: test cases for LinSpace
    Expectation: the result match to numpy
    """
    context.set_context(mode=context.GRAPH_MODE, device_target='CPU')
    start = Tensor(start_np, dtype=mstype.float32)
    stop = Tensor(stop_np, dtype=mstype.float32)
    num = num_np
    result_ms = ops.linspace(start, stop, num).asnumpy()
    result_np = np.linspace(start_np, stop_np, num_np, axis=-1)
    assert np.allclose(result_ms, result_np)


@pytest.mark.level0
@pytest.mark.platform_x86_cpu
@pytest.mark.env_onecard
@pytest.mark.parametrize('start_np, stop_np', [(5, 150), (-25, 147), (-25.3, -147)])
@pytest.mark.parametrize('num_np', [10, 20, 36])
def test_lin_space_net(start_np, stop_np, num_np):
    """
    Feature: ALL To ALL
    Description: test cases for LinSpace Net
    Expectation: the result match to numpy
    """
    context.set_context(mode=context.GRAPH_MODE, device_target='CPU')
    start = Tensor(start_np, dtype=mstype.float32)
    stop = Tensor(stop_np, dtype=mstype.float32)
    net = LinSpaceNet(num_np)
    result_ms = net(start, stop).asnumpy()
    result_np = np.linspace(start_np, stop_np, num_np, axis=-1)
    assert np.allclose(result_ms, result_np)


@pytest.mark.level0
@pytest.mark.platform_x86_cpu
@pytest.mark.env_onecard
@pytest.mark.parametrize('start_np, stop_np', [[[2, 3, 5], [4, 6, 8]], [[-4, 7, -2], [-10, 26, 18]]])
@pytest.mark.parametrize('num_np', [10, 20, 36])
@pytest.mark.parametrize('dtype', [mstype.float32, mstype.float64])
def test_lin_space_batched(start_np, stop_np, num_np, dtype):
    """
    Feature: ALL To ALL
    Description: test cases for LinSpace Net
    Expectation: the result match to numpy
    """
    context.set_context(mode=context.GRAPH_MODE, device_target='CPU')
    start = Tensor(start_np, dtype=mstype.float32)
    stop = Tensor(stop_np, dtype=mstype.float32)
    net = LinSpaceNet(num_np)
    result_ms = net(start, stop).asnumpy()
    result_np = np.linspace(start_np, stop_np, num_np, axis=-1)
    assert np.allclose(result_ms, result_np)
