# Copyright 2021 Huawei Technologies Co., Ltd
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

"""array_ops"""

from .._grad.grad_base import bprop_getters
from ..composite.multitype_ops.zeros_like_impl import zeros_like
from .. import operations as P

@bprop_getters.register(P.TensorScatterSub)
def get_bprop_tensor_scatter_sub(self):
    """Generate bprop for TensorScatterSub"""
    gather_nd = P.GatherNd()

    def bprop(x, indices, update, out, dout):
        update_grad = gather_nd(dout, indices)
        return dout, zeros_like(indices), update_grad

    return bprop


@bprop_getters.register(P.TensorScatterMax)
def get_bprop_tensor_scatter_max(self):
    """Generate bprop for TensorScatterMax"""
    gather_nd = P.GatherNd()
    select = P.Select()
    equal = P.Equal()

    def bprop(x, indices, update, out, dout):
        select_condition = equal(x, out)
        dx = select(select_condition, dout, zeros_like(x))

        possibly_updated_values = gather_nd(out, indices)
        update_loss = gather_nd(dout, indices)
        select_condition = equal(possibly_updated_values, update)
        dupdate = select(select_condition, update_loss, zeros_like(update))

        return dx, zeros_like(indices), dupdate

    return bprop


@bprop_getters.register(P.TensorScatterMin)
def get_bprop_tensor_scatter_min(self):
    """Generate bprop for TensorScatterMin"""
    gather_nd = P.GatherNd()
    select = P.Select()
    equal = P.Equal()

    def bprop(x, indices, update, out, dout):
        select_condition = equal(x, out)
        dx = select(select_condition, dout, zeros_like(x))

        possibly_updated_values = gather_nd(out, indices)
        update_loss = gather_nd(dout, indices)
        select_condition = equal(possibly_updated_values, update)
        dupdate = select(select_condition, update_loss, zeros_like(update))

        return dx, zeros_like(indices), dupdate

    return bprop
