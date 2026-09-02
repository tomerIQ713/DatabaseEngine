#include "sql_common.h"

/* Deliberately silent: the golden baselines capture whatever this prints, so
   it is the one place a stray character invalidates all 30 of them. */
void showBanner(void)
{
}

void showError(int errorCode)
{
    printf("error %d: %s\n", errorCode, errorCodeToString(errorCode));
}
