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

import mindspore.context as context
import mindspore.nn as nn
import mindspore as ms

context.set_context(mode=context.GRAPH_MODE, device_target='CPU')


class OpNetWrapper(nn.Cell):
    def __init__(self, op):
        super(OpNetWrapper, self).__init__()
        self.op = op

    def construct(self, *inputs):
        return self.op(*inputs)


@pytest.mark.level0
@pytest.mark.platform_x86_cpu
@pytest.mark.env_onecard
def test_int():
    op = nn.Range(0, 100, 10)
    op_wrapper = OpNetWrapper(op)

    outputs = op_wrapper()
    print(outputs)
    assert outputs.shape == (10,)
    assert np.allclose(outputs.asnumpy(), range(0, 100, 10))


@pytest.mark.level0
@pytest.mark.platform_x86_cpu
@pytest.mark.env_onecard
def test_float():
    op = nn.Range(10., 100., 20.)
    op_wrapper = OpNetWrapper(op)

    outputs = op_wrapper()
    print(outputs)
    assert outputs.shape == (5,)
    assert np.allclose(outputs.asnumpy(), [10., 30., 50., 70., 90.])


@pytest.mark.level0
@pytest.mark.platform_x86_cpu
@pytest.mark.env_onecard
def test_range_op_int():
    """
    Feature: test Range op on CPU.
    Description: test the Range when input is int.
    Expectation: result is right.
    """
    range_op = ms.ops.Range()
    result = range_op(ms.Tensor(2, ms.int32), ms.Tensor(5, ms.int32), ms.Tensor(2, ms.int32))
    expect = np.array([2, 4], np.int32)
    assert np.array_equal(result.asnumpy(), expect)


@pytest.mark.level0
@pytest.mark.platform_x86_cpu
@pytest.mark.env_onecard
def test_range_op_float():
    """
    Feature: test Range op on CPU.
    Description: test the Range when input is float.
    Expectation: result is right.
    """
    range_op = ms.ops.Range()
    result = range_op(ms.Tensor(2, ms.float32), ms.Tensor(5, ms.float32), ms.Tensor(1, ms.float32))
    expect = np.array([2, 3, 4], np.float32)
    assert np.array_equal(result.asnumpy(), expect)


if __name__ == '__main__':
    test_int()
    test_float()
