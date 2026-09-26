idf_component_register(
    SRCS "board.c" "board_common.c"
    INCLUDE_DIRS "."
    REQUIRES board_utils
    PRIV_REQUIRES driver main
)
