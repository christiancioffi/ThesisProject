idf_component_register(
    SRCS 
        "main.cpp"
        "CNN1D_RNN_10_int8.cpp"
        "MelSpectrogram.cpp" 
        "filter_module.cpp" 
        "mel_matrix.cpp" 
        "inference.cpp" 
        "sd_card.cpp"
        "I2SDriver.cpp"
        "at_commands_parser.cpp"
        "uart_rx_manager.cpp"
        "eg91_sender.cpp"
        "ConfigLoader.cpp"
        "Logger.cpp"
    INCLUDE_DIRS 
        "."
    EMBED_FILES ".api-key" "eg91_config.json"
    PRIV_REQUIRES 
        esp-tflite-micro 
        esp_timer 
        fatfs 
        vfs 
        sdmmc
        esp-dsp
        driver
)
                       