#ifndef COMM_FRAME_CHANNEL_PTHREAD_H
#define COMM_FRAME_CHANNEL_PTHREAD_H

#include "comm_frame_channel.h"
#include "comm_frame_queue_pthread.h"

/*
 * 将一个平台无关帧通道绑定到已经初始化的 pthread 帧队列。
 *
 * 此函数只建立非拥有型绑定，不初始化或销毁 queue。queue 的生命周期必须
 * 长于 channel 的全部使用过程。绑定成功后，通道操作会把 context 转换回
 * comm_frame_queue_pthread_t，并调用对应的 pthread push/pop/close。
 *
 * queue 为空或尚未初始化时失败，且不修改 channel。已经关闭但尚未销毁的
 * queue 仍可以绑定，后续通道操作会按照正常关闭语义返回结果。
 */
comm_frame_channel_result_t comm_frame_channel_pthread_bind(
    comm_frame_channel_t *channel,
    comm_frame_queue_pthread_t *queue);

#endif /* COMM_FRAME_CHANNEL_PTHREAD_H */
