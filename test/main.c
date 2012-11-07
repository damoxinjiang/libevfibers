/********************************************************************

  Copyright 2012 Konstantin Olkhovskiy <lupus@oxnull.net>

  This file is part of libevfibers.

  libevfibers is free software: you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, either version 3 of the
  License, or any later version.

  libevfibers is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General
  Public License along with libevfibers.  If not, see
  <http://www.gnu.org/licenses/>.

 ********************************************************************/

#include <stdlib.h>
#include <check.h>

#include "init.h"
#include "mutex.h"
#include "cond.h"

Suite * evfibers_suite (void)
{
	Suite *s;
	TCase *tc_init, *tc_mutex, *tc_cond;

	s = suite_create ("evfibers");
	tc_init = init_tcase();
	tc_mutex = mutex_tcase();
	tc_cond = cond_tcase();
	suite_add_tcase(s, tc_init);
	suite_add_tcase(s, tc_mutex);
	suite_add_tcase(s, tc_cond);

	return s;
}

int main(void)
{
	int number_failed;
	Suite *s;
	SRunner *sr;

	srand(42);

	s = evfibers_suite();
	sr = srunner_create(s);
	srunner_run_all(sr, CK_NORMAL);
	number_failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
