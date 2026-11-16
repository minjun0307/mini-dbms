#include "test.h"

int g_failures = 0;
int g_checks = 0;

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);  /* 파이프로 출력해도 진행 상황이 바로 보이게 */
    printf("[lexer]\n");
    lexer_tests();
    printf("[parser]\n");
    parser_tests();
    printf("[bptree]\n");
    bptree_tests();
    printf("[engine]\n");
    engine_tests();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
