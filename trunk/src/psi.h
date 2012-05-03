/*****************************************************************************
 *
 *  psi.h
 *
 *  $Id$
 *
 *  Edinburgh Soft Matter and Statistical Physics Group and
 *  Edinburgh Parallel Computing Centre
 *
 *  Kevin Stratford (kevin@epcc.ed.ac.uk)
 *  (c) 2012 The University of Edinburgh
 *
 *****************************************************************************/

#ifndef PSI_H
#define PSI_H

typedef struct psi_s psi_t;

int psi_create(int nk, psi_t ** pobj);
void psi_free(psi_t * obj);
int psi_init_io_info(psi_t * obj, int grid[3]);

int psi_nk(psi_t * obj, int * nk);
int psi_valency(psi_t * obj, int n, int * iv);
int psi_valency_set(psi_t * obj, int n, int iv);
int psi_diffusivity(psi_t * obj, int n, double * diff);
int psi_diffusivity_set(psi_t * obj, int n, double diff);
int psi_halo_psi(psi_t * obj);
int psi_halo_rho(psi_t * obj);

int psi_rho(psi_t * obj, int index, int n, double * rho);
int psi_rho_set(psi_t * obj, int index, int n, double rho);
int psi_psi(psi_t * obj, int index, double * psi);
int psi_psi_set(psi_t * obj, int index, double psi);
int psi_rho_elec(psi_t * obj, int index, double * rho_elec);
int psi_unit_charge(psi_t * obj, double * eunit);
int psi_unit_charge_set(psi_t * obj, double eunit); 
int psi_beta(psi_t * obj, double * beta);
int psi_beta_set(psi_t * obj, double beta);
int psi_epsilon(psi_t * obj, double * epsilon);
int psi_epsilon_set(psi_t * obj, double epsilon);
int psi_bjerrum_length(psi_t * obj, double * lb);

#endif