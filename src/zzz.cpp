#include <random.h>
#include <iostream>
#include <cmath>
#include <chrono>
#include <util/time.h>

std::chrono::microseconds GetExponentialRand(std::chrono::microseconds now, std::chrono::seconds average_interval)
{
    double unscaled = -std::log1p(GetRand(uint64_t{1} << 48) * -0.0000000000000035527136788 /* -1/2^48 */);
    return now + std::chrono::duration_cast<std::chrono::microseconds>(unscaled * average_interval);
}

int main()
{ 
    auto start = GetTime<std::chrono::microseconds>();
    std::chrono::seconds sec{3};
	//    static constexpr auto OUTBOUND_INVENTORY_BROADCAST_INTERVAL{2s};
	//std::chrono::seconds interval=2s;	 
    std::time_t t = std::time(0);  // t is an integer typei
    std::time_t a=GetExponentialRand(start,sec).count(); 

    std::cout << a ;    
    
    //  GetExponentialRand(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count(),sec);

    //std::cout << GetExponentialRand  << std::endl; 
    //std::cout << t << " seconds since 01-Jan-1970\n";
    return 0;
}
