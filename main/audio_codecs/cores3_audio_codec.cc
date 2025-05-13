i2s_chan_config_t chan_cfg = {
    .id = I2S_NUM_0,
    .role = I2S_ROLE_MASTER,
    .dma_desc_num = AudioCodec::DMA_DESC_NUM,
    .dma_frame_num = AudioCodec::DMA_FRAME_NUM,
    .auto_clear_after_cb = true,
    .auto_clear_before_cb = false,
    .intr_priority = 0,
}; 