#include <px4_log.h>
/****************************************************************************
 * @file test_main.cpp
 *
 * @author Thomas Gubler <thomasgubler@gmail.com>
 * @author Mark Charlebois <mcharleb@gmail.com>
 *
 * @brief Test application main file
 *
 ****************************************************************************/
extern "C" __EXPORT int test_app_main(int argc, char *argv[]);

int test_app_main(int argc, char *argv[])
{
	PX4_INFO("Hello from test app!");


	return 0;
}
