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

"""Operations for random number generators."""

from .. import operations as P
from .. import functional as F
from ..primitive import constexpr
from .multitype_ops import _constexpr_utils as const_utils
from ...common import dtype as mstype
from ...common import get_seed as get_global_seed

@constexpr
def get_seed():
    """
    Get the graph-level seed.
    Graph-level seed is used as a global variable, that can be used in different ops in case op-level seed is not set.
    If op-level seed is 0, use graph-level seed; if graph-level seed is also 0, the system would generate a
    random seed.

    Returns:
        Interger. The current graph-level seed.

    Examples:
        >>> C.get_seed()
    """
    global_seed = get_global_seed()
    if global_seed is None:
        return 0
    return global_seed

def normal(shape, mean, stddev, seed=0):
    """
    Generates random numbers according to the Normal (or Gaussian) random number distribution.

    Args:
        shape (tuple): The shape of random tensor to be generated.
        mean (Tensor): The mean μ distribution parameter, which specifies the location of the peak.
          With float32 data type.
        stddev (Tensor): The deviation σ distribution parameter. It should be greater than 0.
          With float32 data type.
        seed (int): Seed is used as entropy source for Random number engines generating pseudo-random numbers.
          Must be non-negative. Default: 0.

    Returns:
        Tensor. The shape should be the broadcasted shape of Input "shape" and shapes of mean and stddev.
        The dtype is float32.

    Examples:
        >>> shape = (4, 16)
        >>> mean = Tensor(1.0, mstype.float32)
        >>> stddev = Tensor(1.0, mstype.float32)
        >>> output = C.normal(shape, mean, stddev, seed=5)
    """
    mean_dtype = F.dtype(mean)
    stddev_dtype = F.dtype(stddev)
    const_utils.check_tensors_dtype_same(mean_dtype, mstype.float32, "normal")
    const_utils.check_tensors_dtype_same(stddev_dtype, mstype.float32, "normal")
    const_utils.check_non_negative("seed", seed, "normal")
    seed1 = get_seed()
    seed2 = seed
    stdnormal = P.StandardNormal(seed1, seed2)
    random_normal = stdnormal(shape)
    value = random_normal * stddev + mean
    return value

def laplace(shape, mean, lambda_param, seed=0):
    r"""
    Generates random numbers according to the Laplace random number distribution.
    It is defined as:

    .. math::
        \text{f}(x;μ,λ) = \frac{1}{2λ}\exp(-\frac{|x-μ|}{λ}),

    Args:
        shape (tuple): The shape of random tensor to be generated.
        mean (Tensor): The mean μ distribution parameter, which specifies the location of the peak.
          With float32 data type.
        lambda_param (Tensor): The parameter used for controling the variance of this random distribution. The
          variance of Laplace distribution is equal to twice the square of lambda_param. With float32 data type.
        seed (int): Seed is used as entropy source for Random number engines generating pseudo-random numbers.
          Default: 0.

    Returns:
        Tensor. The shape should be the broadcasted shape of Input "shape" and shapes of mean and lambda_param.
        The dtype is float32.

    Examples:
        >>> shape = (4, 16)
        >>> mean = Tensor(1.0, mstype.float32)
        >>> lambda_param = Tensor(1.0, mstype.float32)
        >>> output = C.laplace(shape, mean, lambda_param, seed=5)
    """
    mean_dtype = F.dtype(mean)
    lambda_param_dtype = F.dtype(lambda_param)
    const_utils.check_tensors_dtype_same(mean_dtype, mstype.float32, "laplace")
    const_utils.check_tensors_dtype_same(lambda_param_dtype, mstype.float32, "laplace")
    seed1 = get_seed()
    seed2 = seed
    stdlaplace = P.StandardLaplace(seed1, seed2)
    rnd = stdlaplace(shape)
    value = rnd * lambda_param + mean
    return value

def uniform(shape, minval, maxval, seed=0, dtype=mstype.float32):
    """
    Generates random numbers according to the Uniform random number distribution.

    Note:
        The number in tensor minval should be strictly less than maxval at any position after broadcasting.

    Args:
        shape (tuple): The shape of random tensor to be generated.
        minval (Tensor): The a distribution parameter.
          It defines the minimum possibly generated value. With int32 or float32 data type.
          If dtype is int32, only one number is allowed.
        maxval (Tensor): The b distribution parameter.
          It defines the maximum possibly generated value. With int32 or float32 data type.
          If dtype is int32, only one number is allowed.
        seed (int): Seed is used as entropy source for Random number engines generating pseudo-random numbers.
          Must be non-negative. Default: 0.
        dtype (mindspore.dtype): type of the Uniform distribution. If it is int32, it generates numbers from discrete
          uniform distribution; if it is float32, it generates numbers from continuous uniform distribution. It only
          supports these two data types. Default: mstype.float32.

    Returns:
        Tensor. The shape should be the broadcasted shape of Input "shape" and shapes of minval and maxval.
        The dtype is designated as the input `dtype`.

    Examples:
        >>> For discrete uniform distribution, only one number is allowed for both minval and maxval:
        >>> shape = (4, 2)
        >>> minval = Tensor(1, mstype.int32)
        >>> maxval = Tensor(2, mstype.int32)
        >>> output = C.uniform(shape, minval, maxval, seed=5)
        >>>
        >>> For continuous uniform distribution, minval and maxval can be multi-dimentional:
        >>> shape = (4, 2)
        >>> minval = Tensor([1.0, 2.0], mstype.float32)
        >>> maxval = Tensor([4.0, 5.0], mstype.float32)
        >>> output = C.uniform(shape, minval, maxval, seed=5)
    """
    minval_dtype = F.dtype(minval)
    maxval_dtype = F.dtype(maxval)
    const_utils.check_valid_type(dtype, [mstype.int32, mstype.float32], 'uniform')
    const_utils.check_tensors_dtype_same(minval_dtype, dtype, "uniform")
    const_utils.check_tensors_dtype_same(maxval_dtype, dtype, "uniform")
    const_utils.check_non_negative("seed", seed, "uniform")
    seed1 = get_seed()
    seed2 = seed
    if const_utils.is_same_type(dtype, mstype.int32):
        random_uniform = P.UniformInt(seed1, seed2)
        value = random_uniform(shape, minval, maxval)
    else:
        uniform_real = P.UniformReal(seed1, seed2)
        random_uniform = uniform_real(shape)
        value = random_uniform * (maxval - minval) + minval
    return value

def gamma(shape, alpha, beta, seed=0):
    """
    Generates random numbers according to the Gamma random number distribution.

    Args:
        shape (tuple): The shape of random tensor to be generated.
        alpha (Tensor): The alpha α distribution parameter. It should be greater than 0. With float32 data type.
        beta (Tensor): The beta β distribution parameter. It should be greater than 0. With float32 data type.
        seed (int): Seed is used as entropy source for Random number engines generating pseudo-random numbers.
          Must be non-negative. Default: 0.

    Returns:
        Tensor. The shape should be the broadcasted shape of Input "shape" and shapes of alpha and beta.
        The dtype is float32.

    Examples:
        >>> shape = (4, 16)
        >>> alpha = Tensor(1.0, mstype.float32)
        >>> beta = Tensor(1.0, mstype.float32)
        >>> output = C.gamma(shape, alpha, beta, seed=5)
    """
    const_utils.check_non_negative("seed", seed, "gamma")
    seed1 = get_seed()
    seed2 = seed
    random_gamma = P.Gamma(seed1, seed2)
    value = random_gamma(shape, alpha, beta)
    return value

def poisson(shape, mean, seed=0):
    """
    Generates random numbers according to the Poisson random number distribution.

    Args:
        shape (tuple): The shape of random tensor to be generated.
        mean (Tensor): The mean μ distribution parameter. It should be greater than 0. With float32 data type.
        seed (int): Seed is used as entropy source for Random number engines generating pseudo-random numbers.
          Must be non-negative. Default: 0.

    Returns:
        Tensor. The shape should be the broadcasted shape of Input "shape" and shapes of mean.
        The dtype is float32.

    Examples:
        >>> shape = (4, 16)
        >>> mean = Tensor(1.0, mstype.float32)
        >>> output = C.poisson(shape, mean, seed=5)
    """
    const_utils.check_non_negative("seed", seed, "poisson")
    seed1 = get_seed()
    seed2 = seed
    random_poisson = P.Poisson(seed1, seed2)
    value = random_poisson(shape, mean)
    return value

def multinomial(inputs, num_sample, replacement=True, seed=0):
    r"""
    Returns a tensor sampled from the multinomial probability distribution located in the corresponding
    row of tensor input.

    Note:
        The rows of input do not need to sum to one (in which case we use the values as weights),
        but must be non-negative, finite and have a non-zero sum.

    Args:
        inputs (Tensor): the input tensor containing probabilities, must be 1 or 2 dims. With float32 data type.
        num_sample (int): number of samples to draw.
        replacement (bool, optional): whether to draw with replacement or not, default True.
        seed (int, optional): used as entropy source for Random number engines generating pseudo-random numbers.
          Must be non-negative. Default: 0.

    Outputs:
        Tensor. have the same rows with input, each row has num_samples sampled indices.
        The dtype is float32.

    Examples:
        >>> input = Tensor([0, 9, 4, 0], mstype.float32)
        >>> output = C.multinomial(input, 2, True)
    """
    shape = P.Shape()
    reshape = P.Reshape()
    if inputs.dim() != 1 and inputs.dim() != 2:
        const_utils.raise_value_error("inputs dim must be 1d or 2d")
    if not replacement:
        if shape(inputs)[-1] < num_sample:
            const_utils.raise_value_error("num_sample must be less than shape(input)[-1] without replacement")
        n_dist = 1
        if len(shape(inputs)) > 1:
            n_dist = shape(inputs)[-2]
        random_uniform = P.UniformReal(seed=seed)((n_dist * shape(inputs)[-1],))
        if n_dist != 1:
            random_uniform = reshape(random_uniform, (n_dist, shape(inputs)[-1]))
        vals = P.RealDiv()(P.Log()(random_uniform), inputs + 1e-6)
        _, indices = P.TopK()(vals, num_sample)
        return indices
    return P.Multinomial(seed=seed)(inputs, num_sample)
