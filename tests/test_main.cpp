#include "test_framework.h"

#include <spdlog/spdlog.h>

// Cases self-register via TEST_CASE across the other TUs; just run them all.
int main()
{
	spdlog::set_level(spdlog::level::off);
	return ::tf::run();
}
