/*****************************************************************************
 *
 *  phi_gradients.c
 *
 *  Compute various gradients in the order parameter.
 *
 *  $Id: phi_gradients.c,v 1.1.2.3 2008-03-20 18:16:00 kevin Exp $
 *
 *  Edinburgh Soft Matter and Statistical Physics Group and
 *  Edinburgh Parallel Computing Centre
 *
 *  Kevin Stratford (kevin@epcc.ed.ac.uk)
 *  (c) 2007 The University of Edinburgh
 *
 *****************************************************************************/

#include <assert.h>
#include <stdlib.h>

#include "pe.h"
#include "coords.h"
#include "site_map.h"
#include "phi_gradients.h"

extern double * phi_site;
extern double * delsq_phi_site;
extern double * grad_phi_site;
extern double * delsq_delsq_phi_site;
extern double * grad_delsq_phi_site;

/* These are the 'links' used to form the gradients of the order
 * parameter at solid/fluid boundaries. The order could be
 * optimised */

static const int ngrad_ = 27;
static const int bs_cv[27][3] = {{ 0, 0, 0},
				 {-1,-1,-1}, {-1,-1, 0}, {-1,-1, 1},
                                 {-1, 0,-1}, {-1, 0, 0}, {-1, 0, 1},
                                 {-1, 1,-1}, {-1, 1, 0}, {-1, 1, 1},
                                 { 0,-1,-1}, { 0,-1, 0}, { 0,-1, 1},
                                 { 0, 0,-1},             { 0, 0, 1},
				 { 0, 1,-1}, { 0, 1, 0}, { 0, 1, 1},
				 { 1,-1,-1}, { 1,-1, 0}, { 1,-1, 1},
				 { 1, 0,-1}, { 1, 0, 0}, { 1, 0, 1},
				 { 1, 1,-1}, { 1, 1, 0}, { 1, 1, 1}};


static void phi_gradients_with_solid(void);
static void phi_gradients_fluid(void);
static void phi_gradients_fluid_compact(void);
static void phi_gradients_double_fluid(void);

/****************************************************************************
 *
 *  phi_gradients_compute
 *
 *  Depending on the free energy, we may need gradients of the order
 *  parameter...
 *
 ****************************************************************************/

void phi_gradients_compute() {

  /* General case with solid objects */

  phi_gradients_fluid();

  /* Special case for fluid only */
  /* phi_gradients_fluid();*/

  /* Symmetric using div P_ab */

  /* Brazovskii using P_ab */
  /* Brazovskii using div P_ab */

  return;
}

/****************************************************************************
 *
 *  phi_gradients_with_solid
 *
 *  Compute the gradients of the phi field. This is the 'predictor
 *  corrector' method described by Desplat et al. Comp. Phys. Comm.
 *  134, 273--290 (2000) to take account of solid objects.
 *
 *  This calculation can be extended into the halo region by
 *  (nhalo_ - 1) points in each direction.
 *
 ****************************************************************************/

static void phi_gradients_with_solid() {

  int nlocal[3];
  int ic, jc, kc, ic1, jc1, kc1;
  int ia, index, p;
  int nextra = nhalo_ - 1; /* Gradients not computed at last point locally */

  int isite[ngrad_];
  double count[ngrad_];
  double gradt[ngrad_];
  double gradn[3];
  double dphi;
  double rk = 1.0;         /* 1 / free energy penalty parameter kappa */
  const double r9 = (1.0/9.0);
  const double r18 = (1.0/18.0);

  get_N_local(nlocal);
  assert(nhalo_ >= 1);

  for (ic = 1 - nextra; ic <= nlocal[X] + nextra; ic++) {
    for (jc = 1 - nextra; jc <= nlocal[Y] + nextra; jc++) {
      for (kc = 1 - nextra; kc <= nlocal[Z] + nextra; kc++) {

	index = get_site_index(ic, jc, kc);
	if (site_map_get_status(ic, jc, kc) != FLUID) continue;

	/* Set solid/fluid flag to index neighbours */

	for (p = 1; p < ngrad_; p++) {
	  ic1 = ic + bs_cv[p][X];
	  jc1 = jc + bs_cv[p][Y];
	  kc1 = kc + bs_cv[p][Z];

	  isite[p] = get_site_index(ic1, jc1, kc1);
	  if (site_map_get_status(ic1, jc1, kc1) != FLUID) isite[p] = -1;
	}

	for (ia = 0; ia < 3; ia++) {
	  count[ia] = 0.0;
	  gradn[ia] = 0.0;
	}

	for (p = 1; p < ngrad_; p++) {

	  if (isite[p] == -1) continue;
	  dphi = phi_site[isite[p]] - phi_site[index];
	  gradt[p] = dphi;

	  for (ia = 0; ia < 3; ia++) {
	    gradn[ia] += bs_cv[p][ia]*dphi;
	    count[ia] += bs_cv[p][ia]*bs_cv[p][ia];
	  }
	}

	for (ia = 0; ia < 3; ia++) {
	  if (count[ia] > 0.0) gradn[ia] /= count[ia];
	}

	/* Estimate gradient at boundaries */

	for (p = 1; p < ngrad_; p++) {

	  if (isite[p] == -1) {
	    double c, h, phi_b;
	    phi_b = phi_site[index]
	      + 0.5*(bs_cv[p][X]*gradn[X] + bs_cv[p][Y]*gradn[Y]
		     + bs_cv[p][Z]*gradn[Z]);

	    /* Set gradient of phi at boundary following wetting properties */
	    /* C and H are always zero at the moment */

	    c = 0.0; /* site_map_get_H() */
	    h = 0.0; /* site_map_get_C() */

	    gradt[p] = -(c*phi_b + h)*rk;
	  }
	}
 
	/* Accumulate the final gradients */

	dphi = 0.0;
	for (ia = 0; ia < 3; ia++) {
	  gradn[ia] = 0.0;
	}

	for (p = 1; p < ngrad_; p++) {
	  dphi += gradt[p];
	  for (ia = 0; ia < 3; ia++) {
	    gradn[ia] += gradt[p]*bs_cv[p][ia];
	  }
	}

	delsq_phi_site[index] = r9*dphi;
	for (ia = 0; ia < 3; ia++) {
	  grad_phi_site[3*index+ia]  = r18*gradn[ia];
	}

	/* Next site */
      }
    }
  }

  return;
}

/*****************************************************************************
 *
 *  phi_gradients_fluid_compact
 *
 *  Fluid-only gradient calculation. This is a relatively compact
 *  version which can be compared with the above.
 *
 *****************************************************************************/

static void phi_gradients_fluid_compact() {

  int nlocal[3];
  int ic, jc, kc, ic1, jc1, kc1;
  int ia, index, index1, p;
  int nextra = nhalo_ - 1;

  double phi0, phi1;
  const double r9 = (1.0/9.0);
  const double r18 = (1.0/18.0);

  get_N_local(nlocal);
  assert(nhalo_ >= 1);

  for (ic = 1 - nextra; ic <= nlocal[X] + nextra; ic++) {
    for (jc = 1 - nextra; jc <= nlocal[Y] + nextra; jc++) {
      for (kc = 1 - nextra; kc <= nlocal[Z] + nextra; kc++) {

	index = get_site_index(ic, jc, kc);
	phi0 = phi_site[index];

	for (ia = 0; ia < 3; ia++) {
	  grad_phi_site[3*index + ia] = 0.0;
	}
	delsq_phi_site[index] = 0.0;

	for (p = 1; p < ngrad_; p++) {
	  ic1 = ic + bs_cv[p][X];
	  jc1 = jc + bs_cv[p][Y];
	  kc1 = kc + bs_cv[p][Z];
	  index1 = get_site_index(ic1, jc1, kc1);
	  phi1 = phi_site[index1];

	  for (ia = 0; ia < 3; ia++) {
	    grad_phi_site[3*index + ia] += r18*bs_cv[p][ia]*phi1;
	  }
	  delsq_phi_site[index] += r9*(phi1 - phi0);
	}
 
	/* Next site */
      }
    }
  }

  return;
}

/*****************************************************************************
 *
 *  phi_gradients_fluid
 *
 *  Fluid-only gradient calculation. This is an unrolled version.
 *  Ugly, but quick.
 *
 *****************************************************************************/

static void phi_gradients_fluid() {

  int nlocal[3];
  int ic, jc, kc;
  int index;
  int nextra = nhalo_ - 1;
  int xfac, yfac;

  double phi0;
  const double r9 = (1.0/9.0);
  const double r18 = (1.0/18.0);

  get_N_local(nlocal);
  assert(nhalo_ >= 1);

  xfac = (nlocal[Y] + 2*nhalo_)*(nlocal[Z] + 2*nhalo_);
  yfac = (nlocal[Z] + 2*nhalo_);

  for (ic = 1 - nextra; ic <= nlocal[X] + nextra; ic++) {
    /*icm1 = get_site_index
      ic00 = (ic  )*xfac;*/
    for (jc = 1 - nextra; jc <= nlocal[Y] + nextra; jc++) {
      for (kc = 1 - nextra; kc <= nlocal[Z] + nextra; kc++) {

	index = get_site_index(ic, jc, kc);
	phi0 = phi_site[index];

	grad_phi_site[3*index + X] =
	  r18*(phi_site[index+xfac       ]-phi_site[index-xfac       ] +
	       phi_site[index+xfac+yfac+1]-phi_site[index-xfac+yfac+1] +
	       phi_site[index+xfac-yfac+1]-phi_site[index-xfac-yfac+1] +
	       phi_site[index+xfac+yfac-1]-phi_site[index-xfac+yfac-1] +
	       phi_site[index+xfac-yfac-1]-phi_site[index-xfac-yfac-1] +
	       phi_site[index+xfac+yfac  ]-phi_site[index-xfac+yfac  ] +
	       phi_site[index+xfac-yfac  ]-phi_site[index-xfac-yfac  ] +
	       phi_site[index+xfac     +1]-phi_site[index-xfac     +1] +
	       phi_site[index+xfac     -1]-phi_site[index-xfac     -1]);
		    
	grad_phi_site[3*index + Y] = 
	  r18*(phi_site[index     +yfac  ]-phi_site[index     -yfac  ] +
	       phi_site[index+xfac+yfac+1]-phi_site[index+xfac-yfac+1] +
	       phi_site[index-xfac+yfac+1]-phi_site[index-xfac-yfac+1] +
	       phi_site[index+xfac+yfac-1]-phi_site[index+xfac-yfac-1] +
	       phi_site[index-xfac+yfac-1]-phi_site[index-xfac-yfac-1] +
	       phi_site[index+xfac+yfac  ]-phi_site[index+xfac-yfac  ] +
	       phi_site[index-xfac+yfac  ]-phi_site[index-xfac-yfac  ] +
	       phi_site[index     +yfac+1]-phi_site[index     -yfac+1] +
	       phi_site[index     +yfac-1]-phi_site[index     -yfac-1]);
		    
	grad_phi_site[3*index + Z] = 
	  r18*(phi_site[index          +1]-phi_site[index          -1] +
	       phi_site[index+xfac+yfac+1]-phi_site[index+xfac+yfac-1] +
	       phi_site[index-xfac+yfac+1]-phi_site[index-xfac+yfac-1] +
	       phi_site[index+xfac-yfac+1]-phi_site[index+xfac-yfac-1] +
	       phi_site[index-xfac-yfac+1]-phi_site[index-xfac-yfac-1] +
	       phi_site[index+xfac     +1]-phi_site[index+xfac     -1] +
	       phi_site[index-xfac     +1]-phi_site[index-xfac     -1] +
	       phi_site[index     +yfac+1]-phi_site[index     +yfac-1] +
	       phi_site[index     -yfac+1]-phi_site[index     -yfac-1]);
		    
	delsq_phi_site[index]      = r9*(phi_site[index+xfac       ] + 
					 phi_site[index-xfac       ] +
					 phi_site[index     +yfac  ] + 
					 phi_site[index     -yfac  ] +
					 phi_site[index          +1] + 
					 phi_site[index          -1] +
					 phi_site[index+xfac+yfac+1] + 
					 phi_site[index+xfac+yfac-1] + 
					 phi_site[index+xfac-yfac+1] + 
					 phi_site[index+xfac-yfac-1] + 
					 phi_site[index-xfac+yfac+1] + 
					 phi_site[index-xfac+yfac-1] + 
					 phi_site[index-xfac-yfac+1] + 
					 phi_site[index-xfac-yfac-1] +
					 phi_site[index+xfac+yfac  ] + 
					 phi_site[index+xfac-yfac  ] + 
					 phi_site[index-xfac+yfac  ] + 
					 phi_site[index-xfac-yfac  ] + 
					 phi_site[index+xfac     +1] + 
					 phi_site[index+xfac     -1] + 
					 phi_site[index-xfac     +1] + 
					 phi_site[index-xfac     -1] +
					 phi_site[index     +yfac+1] + 
					 phi_site[index     +yfac-1] + 
					 phi_site[index     -yfac+1] + 
					 phi_site[index     -yfac-1] -
					 26.0*phi0);

 
	/* Next site */
      }
    }
  }

  return;
}

/*****************************************************************************
 *
 *  phi_gradients_double_fluid
 *
 *  Computes higher derivatives:
 *    \nabla (\nabla^2 phi)
 *    \nabla^2 \nabla^2 phi
 *
 *  This calculation can be extended (nhalo_ - 2) points beyond the
 *  local system.
 *
 *****************************************************************************/
 
static void phi_gradients_double_fluid() {

  int nlocal[3];
  int ic, jc, kc, ic1, jc1, kc1;
  int ia, index, index1, p;
  int nextra = nhalo_ - 2;

  double phi0, phi1;
  const double r9 = (1.0/9.0);
  const double r18 = (1.0/18.0);

  get_N_local(nlocal);
  assert(nhalo_ >= 2);

  for (ic = 1 - nextra; ic <= nlocal[X] + nextra; ic++) {
    for (jc = 1 - nextra; jc <= nlocal[Y] + nextra; jc++) {
      for (kc = 1 - nextra; kc <= nlocal[Z] + nextra; kc++) {

	index = get_site_index(ic, jc, kc);
	phi0 = delsq_phi_site[index];

	for (ia = 0; ia < 3; ia++) {
	  grad_delsq_phi_site[3*index + ia] = 0.0;
	}
	delsq_delsq_phi_site[index] = 0.0;

	for (p = 1; p < ngrad_; p++) {
	  ic1 = ic + bs_cv[p][X];
	  jc1 = jc + bs_cv[p][Y];
	  kc1 = kc + bs_cv[p][Z];
	  index1 = get_site_index(ic1, jc1, kc1);
	  phi1 = delsq_phi_site[index1];

	  for (ia = 0; ia < 3; ia++) {
	    grad_delsq_phi_site[3*index + ia] += r18*bs_cv[p][ia]*phi1;
	  }
	  delsq_delsq_phi_site[index] += r9*(phi1 - phi0);
	}
 
	/* Next site */
      }
    }
  }

  return;
}

