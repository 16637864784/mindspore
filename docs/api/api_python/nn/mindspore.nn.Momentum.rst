mindspore.nn.Momentum
======================

.. py:class:: mindspore.nn.Momentum(*args, **kwargs)

    Momentum�㷨�Ż�����

    �йظ�����ϸ��Ϣ����������� `On the importance of initialization and momentum in deep learning <https://dl.acm.org/doi/10.5555/3042817.3043064>`_��

    .. math::
            v_{t+1} = v_{t} \ast u + grad

    ��� `use_nesterov` ΪTrue��

    .. math::
            p_{t+1} =  p_{t} - (grad \ast lr + v_{t+1} \ast u \ast lr)

    ��� `use_nesterov` ΪFalse��

    .. math::
            p_{t+1} = p_{t} - lr \ast v_{t+1}

    ���У�:math:`grad` ��:math:`lr` ��:math:`p` ��:math:`v` ��:math:`u` �ֱ��ʾ�ݶȡ�ѧϰ�ʡ��������أ�Moment���Ͷ�����Momentum����

    .. note::
        �ڲ���δ����ʱ���Ż������õ� `weight_decay` Ӧ�������ƺ���"beta"��"gamma"�����������ͨ�������������ɵ���Ȩ��˥�����ԡ�����ʱ��ÿ����������������� `weight_decay` ����δ���ã�������������ʹ���Ż��������õ� `weight_decay` ��

    **������**
        
    - **params (Union[list[Parameter], list[dict]]): ������ `Parameter` ��ɵ��б���ֵ���ɵ��б����б�Ԫ�����ֵ�ʱ���ֵ�ļ�������"params"��"lr"��"weight_decay"��"grad_centralization"��"order_params"��

      -** params** - �����ǰ����Ȩ�أ���ֵ������ `Parameter` �б�
      -** lr** - ��ѡ��������д���"lr"����ʹ�ö�Ӧ��ֵ��Ϊѧϰ�ʡ����û�У���ʹ���Ż��������õ� `learning_rate` ��Ϊѧϰ�ʡ�
      -** weight_decay** - ��ѡ��������д���"weight_decay������ʹ�ö�Ӧ��ֵ��ΪȨ��˥��ֵ�����û�У���ʹ���Ż��������õ� `weight_decay` ��ΪȨ��˥��ֵ��
      -** grad_centralization** - ��ѡ��������д���"grad_centralization"����ʹ�ö�Ӧ��ֵ����ֵ����Ϊ�������͡����û�У�����Ϊ `grad_centralization` ΪFalse���ò����������ھ���㡣
      -** order_params** - ��ѡ����Ӧֵ��Ԥ�ڵĲ�������˳�򡣵�ʹ�ò������鹦��ʱ��ͨ��ʹ�ø�������� `parameters` ��˳�����������ܡ�������д���"order_params"�������Ը��������е���������"order_params"�еĲ���������ĳһ�� `params` �����С�
    
    - **learning_rate (Union[float, int, Tensor, Iterable, LearningRateSchedule]):

      - **float** - �̶���ѧϰ�ʡ�������ڵ����㡣
      - **int** - �̶���ѧϰ�ʡ�������ڵ����㡣�������ͻᱻת��Ϊ��������
      - **Tensor** - �����Ǳ�����һά�����������ǹ̶���ѧϰ�ʡ�һά�����Ƕ�̬��ѧϰ�ʣ���i����ȡ�����е�i��ֵ��Ϊѧϰ�ʡ�
      - **Iterable** - ��̬��ѧϰ�ʡ���i����ȡ��������i��ֵ��Ϊѧϰ�ʡ�
      - **LearningRateSchedule** - ��̬��ѧϰ�ʡ���ѵ�������У��Ż�����ʹ�ò�����step����Ϊ���룬���� `LearningRateSchedule` ʵ�������㵱ǰѧϰ�ʡ�
    
    - **momentum** (float) - ���������͵ĳ��Σ���ʾ�ƶ�ƽ���Ķ�����������ڻ����0.0��
    - **weight_decay** (int, float) - Ȩ��˥����L2 penalty��ֵ��������ڵ���0.0��Ĭ��ֵ��0.0��
    - **loss_scale** (float) - �ݶ�����ϵ�����������0����� `loss_scale` ��������������ת��Ϊ��������ͨ��ʹ��Ĭ��ֵ������ѵ��ʱʹ���� `FixedLossScaleManager`���� `FixedLossScaleManager` �� `drop_overflow_update` ��������ΪFalseʱ����ֵ��Ҫ�� `FixedLossScaleManager` �е� `loss_scale` ��ͬ���йظ�����ϸ��Ϣ�������class��`mindspore.FixedLossScaleManager` ��Ĭ��ֵ��1.0��
    - **use_nesterov** (bool) - �Ƿ�ʹ��Nesterov Accelerated Gradient (NAG)�㷨�����ݶȡ�Ĭ��ֵ��False��

    **���룺**
    
    **gradients** (tuple[Tensor]) - `params` ���ݶȣ���״��shape���� `params` ��ͬ��

    **�����**

    tuple[bool]������Ԫ�ض�ΪTrue��

    **�쳣��**

    - **TypeError** - `learning_rate` ����int��float��Tensor��Iterable��LearningRateSchedule��
    - **TypeError** - `parameters` ��Ԫ�ز��� `Parameter` ���ֵ䡣
    - **TypeError** - `loss_scale` �� `momentum` ����float��
    - **TypeError** - `weight_decay` ����float��int��
    - **TypeError** - `use_nesterov` ����bool��
    - **ValueError** - `loss_scale` С�ڻ����0��
    - **ValueError** - `weight_decay` �� `momentum` С��0��

    **֧��ƽ̨��**
    
    ``Ascend``  ``GPU``  ``CPU``

    **������**
    
    >>> net = Net()
    >>> #1) ���в���ʹ����ͬ��ѧϰ�ʺ�Ȩ��˥��
    >>> optim = nn.Momentum(params=net.trainable_params(), learning_rate=0.1, momentum=0.9)
    >>>
    >>> #2) ʹ�ò������鲢���ò�ͬ��ֵ
    >>> conv_params = list(filter(lambda x: 'conv' in x.name, net.trainable_params()))
    >>> no_conv_params = list(filter(lambda x: 'conv' not in x.name, net.trainable_params()))
    >>> group_params = [{'params': conv_params, 'weight_decay': 0.01, 'grad_centralization':True},
    ...                 {'params': no_conv_params, 'lr': 0.01},
    ...                 {'order_params': net.trainable_params()}]
    >>> optim = nn.Momentum(group_params, learning_rate=0.1, momentum=0.9, weight_decay=0.0)
    >>> # conv_params�����齫ʹ���Ż����е�ѧϰ��0.1�������Ȩ��˥��0.01��������ݶ����Ļ�����True��
    >>> # no_conv_params�����齫ʹ�ø����ѧϰ��0.01���Ż����е�Ȩ��˥��0.0���ݶ����Ļ�ʹ��Ĭ��ֵFalse��
    >>> # �Ż�������"order_params"���õĲ���˳����²�����
    >>>
    >>> loss = nn.SoftmaxCrossEntropyWithLogits()
    >>> model = Model(net, loss_fn=loss, optimizer=optim, metrics=None)