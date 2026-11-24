# Output DSP (EQ, bass enhancement, limiter). Pure C on purpose: the same
# sources build on the host, see scripts/dsp_host_test.sh.
idf_component_register(
    SRCS "dsp.c" "dsp_biquad.c" "dsp_signal.c"
    INCLUDE_DIRS "include"
)

