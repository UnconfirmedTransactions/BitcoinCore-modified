#include <random.h>
#include <iostream>
#include <ctime>

int main()
{
    std::time_t t = std::time(0);  // t is an integer type
    std::cout << t << " seconds since 01-Jan-1970\n";
    return 0;
}
