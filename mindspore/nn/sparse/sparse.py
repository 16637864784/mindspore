# Copyright 2020-2021 Huawei Technologies Co., Ltd
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
"""Sparse related tools."""
from mindspore.ops import operations as P
from ..cell import Cell


class SparseToDense(Cell):
    """
    Convert a sparse tensor into dense.

    Not yet supported by any backend at the moment.

    Args:
        sparse_tensor (SparseTensor): the sparse tensor to convert.

    Returns:
        Tensor, the tensor converted.

    Supported Platforms:
        ``CPU``

    Examples:
        >>> import mindspore as ms
        >>> from mindspore import Tensor, SparseTensor
        >>> import mindspore.nn as nn

        >>> indices = Tensor([[0, 1], [1, 2]])
        >>> values = Tensor([1, 2], dtype=ms.int32)
        >>> dense_shape = (3, 4)
        >>> sparse_tensor = SparseTensor(indices, values, dense_shape)
        >>> sparse_to_dense = nn.SparseToDense()
        >>> result = sparse_to_dense(sparse_tensor)
        >>> print(result)
        [[0 1 0 0]
         [0 0 2 0]
         [0 0 0 0]]
    """
    def __init__(self):
        super(SparseToDense, self).__init__()
        self.sparse_to_dense = P.SparseToDense()

    def construct(self, sparse_tensor):
        return self.sparse_to_dense(sparse_tensor.indices,
                                    sparse_tensor.values,
                                    sparse_tensor.dense_shape)

class SparseTensorDenseMatmul(Cell):
    """
    Multiply SparseTensor(of rank 2) "A" by dense tensor.
    The shape of sparse tensor is :math:`(N, C)`, and the shape of dense tensor is :math:`(C, M)`, then the shape of
    output tensor is :math:`(N, M)`.The output data type is the same as "values".

    Args:
        - *adjoint_st** (Bool) - If true, SparseTensor is transposed before multiplication. Default: False.
        - *adjoint_dt** (Bool) - If true, DenseTensor is transposed before multiplication. Default: False.

    Inputs:
        - **indices** (Tensor) - A 2D tensor with shape [N, rank], containing the indices of the nonzero values. The
          indices of sparse representation, support int32/int64.
        - **values** (Tensor) - A 1D tensor with shape [N] containing all nonzero values. Values corresponding to
          each row of indices.
        - **dense_shape** (tuple) - A 1D tuple with shape (N, M), specifying the shape of the tensor. An int tuple
          which specifies the shape of dense tensor. The dense_shape is : math:`(N, C)`. If `adjoint_st` is True,
          its shape must be :math:`(N, C)` after transpose.
        - **dense** (Tensor) - Dense Matrix. The shape of the tensor is :math:`(C, M)`. If
          `adjoint_dt` is True, its shape must be :math:`(C, M)` after transpose.

    Returns:
        Tensor, the shape of tensor  is :math:`(N, M)`.The output data type is the same as "values".

    Supported Platforms:
        ``CPU``

    Examples:
        >>> class NetSparseDenseMatmul(nn.Cell):
        ...     def __init__(self):
        ...         super(NetSparseDenseMatmul, self).__init__()
        ...         self.matmul = nn.SparseTensorDenseMatmul()
        ...
        ...     def construct(self, indices, values, dens_shape, dt):
        ...         return self.matmul(indices, values, dens_shape, dt)
        ...
        >>> indices = Tensor([[0, 1], [1, 2]], dtype=ms.int32)
        >>> values = Tensor([1, 2], dtype=ms.float32)
        >>> dense_shape = (3, 4)
        >>> dsMatrix = Tensor([[1, 1], [2, 2], [3, 3], [4, 4]], dtype=ms.float32)
        >>> test_SparseDenseMatmul = NetSparseDenseMatmul()
        >>> out = test_SparseDenseMatmul(indices, values, dense_shape, dsMatrix)
        >>> print(out)
        [[2 2]
         [0 6]
         [6 0]]
    """
    def __init__(self, adjoint_st=False, adjoint_dt=False):
        """Initialize SparseTensorDenseMatmul"""
        super(SparseTensorDenseMatmul, self).__init__()
        self.adjst = adjoint_st
        self.adjdt = adjoint_dt
        self.matmul = P.SparseTensorDenseMatmul(adjoint_st=self.adjst, adjoint_dt=self.adjdt)

    def construct(self, indices, values, dense_shape, dense):
        return self.matmul(indices, values, dense_shape, dense)
