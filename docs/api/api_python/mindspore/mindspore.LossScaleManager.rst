mindspore.LossScaleManager
===========================

.. py:class:: mindspore.LossScaleManager

   ��Ͼ����ݶȷŴ�ϵ����loss scale���������ĳ����ࡣ

   ��������Ҫ��������з����� `get_loss_scale` ���ڻ�ȡ��ǰ���ݶȷŴ�ϵ����`update_loss_scale` ���ڸ����ݶȷŴ�ϵ�����÷�������ѵ�������б����á�`get_update_cell` ���ڻ�ȡ�����ݶȷŴ�ϵ���� `Cell` ʵ������ʵ���ڽ�ѵ�������б����á��³�ģʽ�½� `get_update_cell` ��ʽ��Ч�����³�ģʽ�����ָ����ݶȷŴ�ϵ���ķ�ʽ����Ч��
   ���磺:class:`mindspore.FixedLossScaleManager` �� :class:`mindspore.DynamicLossScaleManager` ��
    
    .. py:method:: get_loss_scale()

      ��ȡ�ݶȷŴ�ϵ����loss scale����ֵ��

    .. py:method:: get_update_cell()
      
      ��ȡ���ڸ����ݶȷŴ�ϵ���� :class:`mindspore.nn.Cell` ʵ����

    .. py:method:: update_loss_scale(overflow)

      ���� `overflow` ״̬�����ݶȷŴ�ϵ����loss scale)��

      **������**

      **overflow** (bool) - ��ʾѵ�������Ƿ������
        