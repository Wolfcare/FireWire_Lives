#include "test_util.h"

int g_tests_run = 0;
int g_tests_failed = 0;
const char *g_current_suite = "";

int main(void)
{
    printf("FireWire_Lives core test suite\n");
    printf("==============================\n");

    test_crc();
    test_packet();
    test_transaction();
    test_selfid();
    test_configrom();
    test_sbp2();

    printf("\n==============================\n");
    if (g_tests_failed == 0)
        printf("\033[32mAll %d checks passed.\033[0m\n", g_tests_run);
    else
        printf("\033[31m%d of %d checks FAILED.\033[0m\n",
               g_tests_failed, g_tests_run);

    return g_tests_failed == 0 ? 0 : 1;
}
