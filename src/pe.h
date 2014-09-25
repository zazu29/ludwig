/*****************************************************************************
 *
 *  pe.h
 *
 *  (c) 2010-2014 The University of Edinburgh
 *
 *  Edinburgh Soft Matter and Statistical Physics Group and
 *  Edinburgh Parallel Computing Centre
 *  Kevin Stratford (kevin@epcc.ed.ac.uk)
 *
 *****************************************************************************/

#ifndef PE_H
#define PE_H

#include "../version.h"
#include <mpi.h>

typedef struct pe_s pe_t;

void pe_init(void);
void pe_finalise(void);
int  pe_init_quiet(void);
int  pe_rank(void);
int  pe_size(void);
void pe_parent_comm_set(const MPI_Comm parent);
void pe_redirect_stdout(const char * filename);
void pe_subdirectory_set(const char * name);
void pe_subdirectory(char * name);

MPI_Comm pe_comm(void);

void info(const char * fmt, ...);
void fatal(const char * fmt, ...);
void verbose(const char * fmt, ...);

#endif
