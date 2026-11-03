/*
This file is part of Spindle.  For copyright information see the COPYRIGHT 
file in the top level directory, or at 
https://github.com/hpc/Spindle/blob/master/COPYRIGHT

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License (as published by the Free Software
Foundation) version 2.1 dated February 1999.  This program is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY; without even the IMPLIED
WARRANTY OF MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms 
and conditions of the GNU Lesser General Public License for more details.  You should 
have received a copy of the GNU Lesser General Public License along with this 
program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#ifndef _COBO_FAULT_INJECT_H
#define _COBO_FAULT_INJECT_H

#include "ldcs_cobo.h"

#if defined(COBO_NAMESPACE)
#define cobo_test_fault_point COMBINE(COBO_NAMESPACE, cobo_test_fault_point)
#define cobo_test_exit_hosts COMBINE(COBO_NAMESPACE, cobo_test_exit_hosts)
#endif

/* Fault injection for startup resiliency testing */
void cobo_test_fault_point(const char *phase, int rank);
void cobo_test_exit_hosts(void);

#endif
