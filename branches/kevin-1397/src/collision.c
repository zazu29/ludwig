/*****************************************************************************
 *
 *  collision.c
 *
 *  Collision stage routines and associated data.
 *
 *  Isothermal fluctuations following Adhikari et al., Europhys. Lett
 *  (2005).
 *
 *  The relaxation times can be set to give either 'm10', BGK or
 *  'two-relaxation' time (TRT) models.
 *
 *  Edinburgh Soft Matter and Statistical Physics Group and
 *  Edinburgh Parallel Computing Centre
 *
 *  (c) 2011-2016 The University of Edinburgh
 *
 *  Contributing authors:
 *    Kevin Stratford (kevin@epcc.ed.ac.uk)
 *    Alan Gray (alang@epcc.ed.ac.uk)
 *
 *****************************************************************************/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "pe.h"
#include "util.h"
#include "coords.h"
#include "physics.h"
#include "model.h"
#include "lb_model_s.h"
#include "hydro_s.h"
#include "free_energy.h"
#include "control.h"
#include "collision.h"
#include "field_s.h"
#include "map_s.h"
#include "timer.h"

#include "symmetric.h"

static int nrelax_ = RELAXATION_M10;     /* [RELAXATION_M10|TRT|BGK] */
                                         /* Default is M10 */

static double rtau_shear;       /* Inverse relaxation time for shear modes */
static double rtau_bulk;        /* Inverse relaxation time for bulk modes */

static int lb_collision_mrt(lb_t * lb, hydro_t * hydro, map_t * map, noise_t * noise);
static int lb_collision_binary(lb_t * lb, hydro_t * hydro, map_t * map,
			       noise_t * noise, fe_symm_t * fe);

static __host__ __device__ __inline__ 
void lb_collision_fluctuations(lb_t * lb, noise_t * noise, int index,
			       double shat[3][3], double ghat[NVEL]);


/* TODO refactor these type definitions and forward declarations */


__target__ void d3q19matmult(double* mode, const double* __restrict__ ftmp_d, int ndist, int baseIndex);
__target__ void d3q19matmult2(double* mode, double* f_d, int ndist, int baseIndex);


__target__ void d3q19matmultchunk(double* mode, const double* __restrict__ fchunk, int baseIndex);
__target__ void d3q19matmult2chunk(double* mode, double* fchunk, int baseIndex);

__target__ void updateDistD3Q19(double jdotc[3*VVL],double sphidotq[VVL],double sphi[3][3*VVL],double phi[VVL],double jphi[3*VVL],double* t_f,int baseIndex);



/* Constants */

__targetConst__ int tc_nSites;
__targetConst__ int tc_noffset[3]; 
__targetConst__ int tc_Nall[3];
__targetConst__ int tc_ntotal[3];
__targetConst__ int tc_periodic[3];
__targetConst__ int tc_nhalo;
__targetConst__ int tc_nextra;

__targetConst__ double tc_rtau_shear;
__targetConst__ double tc_rtau_bulk;
__targetConst__ double tc_rtau_[NVEL];
__targetConst__ double tc_wv[NVEL];
__targetConst__ double tc_ma_[NVEL][NVEL];
__targetConst__ double tc_mi_[NVEL][NVEL];
__targetConst__ double tc_rtau2;
__targetConst__ double tc_rcs2;
__targetConst__ double tc_r2rcs4;
__targetConst__ double tc_force_global[3];
__targetConst__ double tc_q_[NVEL][3][3];

/*****************************************************************************
 *
 *  lb_collide
 *
 *  Driver routine for the collision stage.
 *
 *  We allow hydro to be NULL, in which case there is no hydrodynamics!
 * 
 *****************************************************************************/

__host__
int lb_collide(lb_t * lb, hydro_t * hydro, map_t * map, noise_t * noise,
	       fe_t * fe) {

  int ndist;

  if (hydro == NULL) return 0;

  assert(lb);
  assert(map);

  lb_ndist(lb, &ndist);
  lb_collision_relaxation_times_set(lb, noise);
  lb_collide_param_commit(lb);

  if (ndist == 1) lb_collision_mrt(lb, hydro, map, noise);
  if (ndist == 2) lb_collision_binary(lb, hydro, map, noise,
				      (fe_symm_t *) fe);

  return 0;
}


/*****************************************************************************
 *
 *  lb_collision_mrt
 *
 *  Collision with (potentially) different relaxation times for each
 *  different mode.
 *
 *  This code is per lattice site. To be called from
 *  lb_collision_mrt_lattice().
 *
 *  The matrices ma_ and mi_ project the distributions onto the
 *  modes, and vice-versa, respectively, for the current LB model.
 *
 *  The collision conserves density, and momentum (to within any
 *  body force present). The stress modes, and ghost modes, are
 *  relaxed toward their equilibrium values.
 *
 *****************************************************************************/

__target__ void lb_collision_mrt_site(lb_t * lb, 
				      hydro_t * hydro, map_t * map,
				      noise_t * noise, int noise_on,
				      const int baseIndex){
  
  int p, m;                        /* velocity index */
  int ia, ib;                      /* indices ("alphabeta") */
  int iv=0;                          /* SIMD loop counter */
  double mode[NVEL*VVL];           /* Modes; hydrodynamic + ghost */
  double rho[VVL], rrho[VVL];      /* Density, reciprocal density */
  double u[3*VVL];                 /* Velocity */
  double s[3][3*VVL];              /* Stress */
  double seq[3][3*VVL];            /* Equilibrium stress */
  double shat[3][3*VVL];           /* random stress */
  double ghat[NVEL*VVL];           /* noise for ghosts */
  double rdim;                     /* 1 / dimension */

  double force[3*VVL];             /* External force */
  double tr_s[VVL], tr_seq[VVL];   /* SIMD vectors for stress trace */
  double fchunk[NVEL*VVL];         /* SIMD distribution vector */

  char fullchunk=1;
  char includeSite[VVL];
  KRONECKER_DELTA_CHAR(d);

  assert(lb);
  assert(lb->p);
  assert(hydro);

  /* Determine whether this chunk of lattice sites are all active
   * and if not, which should be included */

  __targetILP__(iv) includeSite[iv] = 1;

  __targetILP__(iv) {
    if ((baseIndex+iv) >= tc_nSites) {
      includeSite[iv] = 0;
      fullchunk = 0;
    }
    else {
      if (map->status[baseIndex+iv] != MAP_FLUID) {
	includeSite[iv] = 0;
	fullchunk = 0;
      }
    }
  }

  /* Default to fluctuations off; shat, ghat are zero */
  for (ia = 0; ia < 3; ia++) {
    for (ib = 0; ib < 3; ib++) {
      __targetILP__(iv) shat[ia][ib*VVL+iv] = 0.0;
    }
  }

  for (ia = 0; ia < NVEL; ia++) {
    __targetILP__(iv) ghat[ia*VVL+iv] = 0.0;
  }

  rdim = 1.0/NDIM;


  /* Load SIMD vectors for distribution and force */

  if (fullchunk) {
    /* distribution */
    for (p = 0; p < NVEL; p++) {
      __targetILP__(iv) fchunk[p*VVL+iv] = 
	lb->f[ LB_ADDR(tc_nSites, 1, NVEL, baseIndex + iv, 0, p) ];
    }
    /* force */

    for (ia = 0; ia < 3; ia++) {
      __targetILP__(iv) {
	force[ia*VVL+iv] = tc_force_global[ia] 
	  + hydro->f[addr_rank1(hydro->nsite, NHDIM, baseIndex+iv, ia)];
      }
    }
  }
  else {
    __targetILP__(iv) {
      if (includeSite[iv]) {
	/* distribution */
	for (p = 0; p < NVEL; p++) {
	  fchunk[p*VVL+iv] = 
	    lb->f[ LB_ADDR(tc_nSites, 1, NVEL, baseIndex + iv, 0, p) ];
	}

	/* force */

	for (ia = 0; ia < 3; ia++) {
	  force[ia*VVL+iv] = tc_force_global[ia] 
	    + hydro->f[addr_rank1(hydro->nsite, NHDIM, baseIndex+iv, ia)];
	}
      }
    }
  }
  
  /* Compute all the modes */

#ifdef _D3Q19_
    d3q19matmultchunk(mode, fchunk, baseIndex);
#else
    for (m = 0; m < NVEL; m++) {
      __targetILP__(iv) mode[m*VVL+iv] = 0.0;
      for (p = 0; p < NVEL; p++) {
	__targetILP__(iv) mode[m*VVL+iv] += fchunk[p*VVL+iv]*tc_ma_[m][p];
      }
    }
#endif

  /* For convenience, write out the physical modes, that is,
   * rho, NDIM components of velocity, independent components
   * of stress (upper triangle), and lower triangle. */

  __targetILP__(iv) rho[iv] = mode[0*VVL+iv];
  for (ia = 0; ia < NDIM; ia++) {
    __targetILP__(iv) u[ia*VVL+iv] = mode[(1 + ia)*VVL+iv];
  }

  m = 0;
  for (ia = 0; ia < NDIM; ia++) {
    for (ib = ia; ib < NDIM; ib++) {
      __targetILP__(iv) s[ia][ib*VVL+iv] = mode[(1 + NDIM + m)*VVL+iv];
      m++;
    }
  }
    
  for (ia = 1; ia < NDIM; ia++) {
    for (ib = 0; ib < ia; ib++) {
      __targetILP__(iv) s[ia][ib*VVL+iv] = s[ib][ia*VVL+iv];
    }
  }

  /* Compute the local velocity, taking account of any body force */
    
  __targetILP__(iv) rrho[iv] = 1.0/rho[iv];

  for (ia = 0; ia < NDIM; ia++) {      
    __targetILP__(iv) {
      u[ia*VVL+iv] = rrho[iv]*(u[ia*VVL+iv] + 0.5*force[ia*VVL+iv]);  
    }
  }
   
  /* Relax stress with different shear and bulk viscosity */

  __targetILP__(iv) {
    tr_s[iv]   = 0.0;
    tr_seq[iv] = 0.0;
  }
    
  for (ia = 0; ia < NDIM; ia++) {
    /* Set equilibrium stress */
    for (ib = 0; ib < NDIM; ib++) {
      __targetILP__(iv) {
	seq[ia][ib*VVL+iv] = rho[iv]*u[ia*VVL+iv]*u[ib*VVL+iv];
      }
    }
    /* Compute trace */
    __targetILP__(iv){
      tr_s[iv]   += s[ia][ia*VVL+iv];
      tr_seq[iv] += seq[ia][ia*VVL+iv];
    }
  }
    
  /* Form traceless parts */
  for (ia = 0; ia < NDIM; ia++) {
    __targetILP__(iv){
      s[ia][ia*VVL+iv]   -= rdim*tr_s[iv];
      seq[ia][ia*VVL+iv] -= rdim*tr_seq[iv];
    }
  }
    
  /* Relax each mode */
  __targetILP__(iv) {
    tr_s[iv] = tr_s[iv] - tc_rtau_bulk*(tr_s[iv] - tr_seq[iv]);
  }

  for (ia = 0; ia < NDIM; ia++) {
    for (ib = 0; ib < NDIM; ib++) {
      __targetILP__(iv) {
	s[ia][ib*VVL+iv] -= tc_rtau_shear*(s[ia][ib*VVL+iv] - seq[ia][ib*VVL+iv]);
	s[ia][ib*VVL+iv] += d[ia][ib]*rdim*tr_s[iv];
	  
	/* Correction from body force (assumes equal relaxation times) */
	      
	s[ia][ib*VVL+iv] += (2.0-tc_rtau_shear)*(u[ia*VVL+iv]*force[ib*VVL+iv]
			  + force[ia*VVL+iv]*u[ib*VVL+iv]);
      }
    }
  }

  if (noise_on) {
	
    double shat1[3][3];
    double ghat1[NVEL];

    /* This does not vectorise at the moment. Needs revisiting */

    for (iv = 0; iv < NSIMDVL; iv++) {

      if (includeSite[iv]) {

	lb_collision_fluctuations(lb, noise, baseIndex + iv, shat1, ghat1);

	for (ia = 0; ia < NDIM; ia++) {
	  for (ib = 0; ib < NDIM; ib++) {
	    shat[ia][ib*VVL+iv] = shat1[ia][ib];
	  }
	}

	for (ia = 0; ia < NVEL; ia++) {
	  ghat[ia*VVL+iv] = ghat1[ia];
	}
      }
    }
  }


  /* Now reset the hydrodynamic modes to post-collision values:
   * rho is unchanged, velocity unchanged if no force,
   * independent components of stress, and ghosts. */
    
  for (ia = 0; ia < NDIM; ia++) {
    __targetILP__(iv) mode[(1 + ia)*VVL+iv] += force[ia*VVL+iv];
  }
    
  m = 0;
  for (ia = 0; ia < NDIM; ia++) {
    for (ib = ia; ib < NDIM; ib++) {
      __targetILP__(iv) {
	mode[(1 + NDIM + m)*VVL+iv] = s[ia][ib*VVL+iv] + shat[ia][ib*VVL+iv];
      }
      m++;
    }
  }

  /* Ghost modes are relaxed toward zero equilibrium. */

  for (m = NHYDRO; m < NVEL; m++) {  
    __targetILP__(iv) {
      mode[m*VVL+iv] = mode[m*VVL+iv] - lb->p->rtau[m]*(mode[m*VVL+iv] - 0.0)
	                              + ghat[m*VVL+iv];
    }
  }


  /* Project post-collision modes back onto the distribution */
#ifdef _D3Q19_
  d3q19matmult2chunk(mode, fchunk, baseIndex);
#else
    for (p = 0; p < NVEL; p++) {
      double ftmp[VVL];
      __targetILP__(iv) ftmp[iv] = 0.0;
      for (m = 0; m < NVEL; m++) {
	__targetILP__(iv) ftmp[iv] += tc_mi_[p][m]*mode[m*VVL+iv];
      }
      __targetILP__(iv) fchunk[p*VVL+iv] = ftmp[iv];
    }
#endif


  /* Write SIMD chunks back to main arrays. */

  if (fullchunk) {
    /* distribution */
    for (p = 0; p < NVEL; p++) {
      __targetILP__(iv) { 
	lb->f[ LB_ADDR(tc_nSites, 1, NVEL, baseIndex + iv, 0, p) ]
	  = fchunk[p*VVL+iv];
      }
    }
    /* velocity */
    for (ia = 0; ia < 3; ia++) {
      __targetILP__(iv) {
	hydro->u[addr_rank1(hydro->nsite, NHDIM, baseIndex+iv, ia)] = u[ia*VVL+iv];
      }
    }
  }
  else {
    __targetILP__(iv) {
      if (includeSite[iv]) {
	/* distribution */
	for (p = 0; p < NVEL; p++) {
	  lb->f[ LB_ADDR(tc_nSites, 1, NVEL, baseIndex + iv, 0, p) ]
	    = fchunk[p*VVL+iv]; 
	}
	/* velocity */

	for (ia = 0; ia < 3; ia++) {
	  hydro->u[addr_rank1(hydro->nsite, NHDIM, baseIndex + iv, ia)] = u[ia*VVL+iv];
	}
      }
    }
  }

  return;
}

/*****************************************************************************
 *
 *  lb_collision_mrt_lattice
 *
 *  Kernel driver for thread-decomposed collision routine; this generates
 *  a loop over all lattice sites.
 *
 *****************************************************************************/

__targetEntry__ void lb_collision_mrt_lattice(lb_t * lb, 
					      hydro_t * hydro, 
					      map_t * map,
					      noise_t * noise, int noise_on,
					      int nSites) {
  int baseIndex = 0;

  __targetTLP__(baseIndex, nSites) {
    lb_collision_mrt_site(lb, hydro, map, noise,
			  noise_on, baseIndex);
  }


  return;
}

/*****************************************************************************
 *
 *  lb_collision_mrt
 *
 *  Single fluid collision driver (multiple relaxation time).
 *
 *****************************************************************************/

int lb_collision_mrt(lb_t * lb, hydro_t * hydro, map_t * map, noise_t * noise) {

  int nhalo;
  int nlocal[3];
  int Nall[3];
  int nSites;
  int noise_on = 0;
  double force_global[3];
  physics_t * phys = NULL;

  assert(lb);
  assert(hydro);
  assert(map);

  nhalo = coords_nhalo();
  coords_nlocal(nlocal);
  physics_ref(&phys);
  physics_fbody(phys, force_global);

  Nall[X] = nlocal[X]+2*nhalo;
  Nall[Y] = nlocal[Y]+2*nhalo;
  Nall[Z] = nlocal[Z]+2*nhalo;
  nSites  = Nall[X]*Nall[Y]*Nall[Z];

  copyConstToTarget(&tc_rtau_shear, &rtau_shear, sizeof(double));
  copyConstToTarget(&tc_rtau_bulk, &rtau_bulk, sizeof(double));
  copyConstToTarget(tc_wv, wv, NVEL*sizeof(double));
  copyConstToTarget(tc_ma_, ma_, NVEL*NVEL*sizeof(double));
  copyConstToTarget(tc_mi_, mi_, NVEL*NVEL*sizeof(double));
  copyConstToTarget(tc_cv, cv, NVEL*3*sizeof(int));
  copyConstToTarget(&tc_rcs2, &rcs2, sizeof(double));
  copyConstToTarget(&tc_nSites,&nSites, sizeof(int));
  copyConstToTarget(&tc_nhalo,&nhalo, sizeof(int));
  copyConstToTarget(tc_force_global,force_global, 3*sizeof(double));
  copyConstToTarget(tc_q_, q_, NVEL*3*3*sizeof(double));
  copyConstToTarget(tc_Nall, Nall, 3*sizeof(int));

  checkTargetError("constants");

  noise_present(noise, NOISE_RHO, &noise_on);

  TIMER_start(TIMER_COLLIDE_KERNEL);

  lb_collision_mrt_lattice __targetLaunch__(nSites) ( lb->target, hydro->target, map->target, noise,noise_on,nSites);

  targetSynchronize();

  TIMER_stop(TIMER_COLLIDE_KERNEL);

  return 0;
}


/*****************************************************************************
 *
 *  lb_collision_binary
 *
 *  Binary LB collision stage (here we are progressing toward
 *  decoupled version).
 *
 *  This follows the single fluid version above, with the addition
 *  that the equilibrium stress includes the thermodynamic term
 *  following Swift etal.
 *
 *  We also have to update the second distribution g from the
 *  order parameter modes phi, jphi[3], sphi[3][3].
 *
 *  There are two choices:
 *    1. relax jphi[i] toward equilibrium phi*u[i] at rate rtau2
 *       AND
 *       fix sphi[i][j] = phi*u[i]*u[j] + mu*d_[i][j]
 *       so the mobility enters through rtau2 (J. Stat. Phys. 2005).
 *    2.
 *       fix jphi[i] = phi*u[i] (i.e. relaxation time == 1.0)
 *       AND
 *       fix sphi[i][j] = phi*u[i]*u[j] + mobility*mu*d_[i][j]
 *       so the mobility enters with chemical potential (Kendon etal 2001).
 *
 *       Note thare is an extra factor of cs^2 before the mobility,
 *       which should be taken into account if quoting the actual
 *       mobility. The factor is somewhat arbitrary and is there
 *       to try to ensure both methods are stable for given input
 *       mobility.
 *
 *   As there seems to be little to choose between the two in terms of
 *   results, I prefer 2, as it avoids the calculation of jphi[i] from
 *   from the distributions g. However, keep 1 so tests don't break!
 *
 *   However, for asymmetric quenches version 1 may be preferred.
 *
 *   The reprojection of g moves phi (mostly) into the non-propagating
 *   distribution following J. Stat. Phys. (2005).
 *
 *****************************************************************************/

#define NDIST 2 /* for binary collision */


__target__ void lb_collision_binary_site(lb_t * lb, 
					 hydro_t * hydro,
					 fe_symm_t * fe,
					 noise_t * noise, int noise_on,
					 const int baseIndex){


  int i, ia, j,m,p;

  
  double mode[NVEL*VVL]; /* Modes; hydrodynamic + ghost */
  
  /* Density, reciprocal density */
  double rho[VVL]; 
  double rrho[VVL];
    
  double u[3*VVL];       /* Velocity */
  double s[3][3*VVL];       /* Stress */
  double seq[3][3*VVL];     /* equilibrium stress */
  double shat[3][3*VVL];    /* random stress */
  double ghat[NVEL*VVL]; /* noise for ghosts */
  
  double force[3*VVL];  /* External force */

  double tr_s[VVL]; 
  double tr_seq[VVL];
  /* modes */
  double phi[VVL]; 
  double jdotc[VVL]; 
  double sphidotq[VVL];     

  double jphi[3*VVL]; 

  double sth[3][3][NSIMDVL];
  double sphi[3][3*VVL];

  double mu[VVL];   /* Chemical potential */
  const double r3 = 1.0/3.0;
  KRONECKER_DELTA_CHAR(d);

  /* index for SIMD vectors */
  int iv=0;        

  assert(lb);
  assert(hydro);

  /* switch fluctuations off */
  for (i = 0; i < 3; i++) {
    for (j = 0; j < 3; j++) {
      __targetILP__(iv) shat[i][j*VVL+iv] = 0.0;
    }
  }

  for (i = NHYDRO; i < NVEL; i++) {
    __targetILP__(iv) ghat[i*VVL+iv] = 0.0;
  }


#ifdef _D3Q19_
  d3q19matmult(mode, lb->f, 2, baseIndex);
#else
    /* Compute all the modes */
    for (m = 0; m < NVEL; m++) {
      __targetILP__(iv) mode[m*VVL+iv] = 0.0;
      for (p = 0; p < NVEL; p++) {
	__targetILP__(iv) mode[m*VVL+iv] +=
	  lb->f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex + iv, 0, p) ]
	  *tc_ma_[m][p];
      }
    }
#endif

  /* For convenience, write out the physical modes. */
  
  __targetILP__(iv) rho[iv] = mode[0*VVL+iv];
  for (i = 0; i < 3; i++) {
    __targetILP__(iv) u[i*VVL+iv] = mode[(1 + i)*VVL+iv];
  }
  
  __targetILP__(iv) {
    s[X][X*VVL+iv] = mode[4*VVL+iv];
    s[X][Y*VVL+iv] = mode[5*VVL+iv];
    s[X][Z*VVL+iv] = mode[6*VVL+iv];
    s[Y][X*VVL+iv] = s[X][Y*VVL+iv];
    s[Y][Y*VVL+iv] = mode[7*VVL+iv];
    s[Y][Z*VVL+iv] = mode[8*VVL+iv];
    s[Z][X*VVL+iv] = s[X][Z*VVL+iv];
    s[Z][Y*VVL+iv] = s[Y][Z*VVL+iv];
    s[Z][Z*VVL+iv] = mode[9*VVL+iv];
  }

  /* Compute the local velocity, taking account of any body force */
  
  __targetILP__(iv) rrho[iv] 
    = 1.0/rho[iv];


  
  for (i = 0; i < 3; i++) {

    __targetILP__(iv) {
      force[i*VVL+iv] = tc_force_global[i] 
	+ hydro->f[addr_rank1(hydro->nsite, NHDIM, baseIndex +iv, i)];
      u[i*VVL+iv] = rrho[iv]*(u[i*VVL+iv] + 0.5*force[i*VVL+iv]);  
    }
  }
  

  for (ia = 0; ia < 3; ia++) {   
    __targetILP__(iv) {
      hydro->u[addr_rank1(hydro->nsite, NHDIM, baseIndex + iv, ia)] = u[ia*VVL+iv];
    }
  }
  
  /* Compute the thermodynamic component of the stress */

  fe_symm_str_v(fe, baseIndex, sth);

  /* Relax stress with different shear and bulk viscosity */
  
  __targetILP__(iv){
    tr_s[iv]   = 0.0;
    tr_seq[iv] = 0.0;
  }
  
  for (i = 0; i < 3; i++) {
    /* Set equilibrium stress, which includes thermodynamic part */
    for (j = 0; j < 3; j++) {
      __targetILP__(iv) seq[i][j*VVL+iv] = rho[iv]*u[i*VVL+iv]*u[j*VVL+iv] 
	+ sth[i][j][iv];
    }
    /* Compute trace */
    __targetILP__(iv) {
      tr_s[iv]   += s[i][i*VVL+iv];
      tr_seq[iv] += seq[i][i*VVL+iv];
    }
  }
  
  /* Form traceless parts */
  for (i = 0; i < 3; i++) {
    __targetILP__(iv) {
      s[i][i*VVL+iv]   -= r3*tr_s[iv];
      seq[i][i*VVL+iv] -= r3*tr_seq[iv];
    }
  }

  
  /* Relax each mode */
  __targetILP__(iv)
    tr_s[iv] = tr_s[iv] - tc_rtau_bulk*(tr_s[iv] - tr_seq[iv]);
  
  for (i = 0; i < 3; i++) {
    for (j = 0; j < 3; j++) {

      __targetILP__(iv) {
	s[i][j*VVL+iv] -= tc_rtau_shear*(s[i][j*VVL+iv] - seq[i][j*VVL+iv]);
	s[i][j*VVL+iv] += d[i][j]*r3*tr_s[iv];
      
	/* Correction from body force (assumes equal relaxation times) */
      
	s[i][j*VVL+iv] += (2.0-tc_rtau_shear)*(u[i*VVL+iv]*force[j*VVL+iv] 
					   + force[i*VVL+iv]*u[j*VVL+iv]);
      }
    }
  }
  

  if (noise_on) {

    /* Not vectorised */

    for (iv = 0; iv < NSIMDVL; iv++) {
      
      double shat1[3][3];
      double ghat1[NVEL];

      lb_collision_fluctuations(lb, noise, baseIndex + iv, shat1, ghat1);

      for (i = 0; i < 3; i++) {
	for (j = 0; j < 3; j++) {
	  shat[i][j*VVL+iv] = shat1[i][j];
	}
      }
      for (p = 0; p < NVEL; p++) {
	ghat[p*VVL+iv] = ghat1[p];
      }
    }    
  }    
  
  /* Now reset the hydrodynamic modes to post-collision values */
  
  __targetILP__(iv) {
    mode[1*VVL+iv] = mode[1*VVL+iv] + force[X*VVL+iv];    /* Conserved if no force */
    mode[2*VVL+iv] = mode[2*VVL+iv] + force[Y*VVL+iv];    /* Conserved if no force */
    mode[3*VVL+iv] = mode[3*VVL+iv] + force[Z*VVL+iv];    /* Conserved if no force */
    mode[4*VVL+iv] = s[X][X*VVL+iv] + shat[X][X*VVL+iv];
    mode[5*VVL+iv] = s[X][Y*VVL+iv] + shat[X][Y*VVL+iv];
    mode[6*VVL+iv] = s[X][Z*VVL+iv] + shat[X][Z*VVL+iv];
    mode[7*VVL+iv] = s[Y][Y*VVL+iv] + shat[Y][Y*VVL+iv];
    mode[8*VVL+iv] = s[Y][Z*VVL+iv] + shat[Y][Z*VVL+iv];
    mode[9*VVL+iv] = s[Z][Z*VVL+iv] + shat[Z][Z*VVL+iv];
  }
  
  
  /* Ghost modes are relaxed toward zero equilibrium. */

  for (m = NHYDRO; m < NVEL; m++) { 
    __targetILP__(iv)  mode[m*VVL+iv] = mode[m*VVL+iv] 
	- lb->p->rtau[m]*(mode[m*VVL+iv] - 0.0) + ghat[m*VVL+iv];
  }

  /* Project post-collision modes back onto the distribution */

#ifdef _D3Q19_  
  d3q19matmult2(mode, lb->f,2, baseIndex);
#else    
    for (p = 0; p < NVEL; p++) {
      double ftmp[VVL];
      __targetILP__(iv) ftmp[iv]=0.;
      for (m = 0; m < NVEL; m++) {
	__targetILP__(iv) ftmp[iv] += tc_mi_[p][m]*mode[m*VVL+iv];
      }
      __targetILP__(iv) lb->f[ LB_ADDR(tc_nSites, NDIST, 
				     NVEL, baseIndex+iv, 
				     0, p) ] = ftmp[iv];
    }
#endif
  
  

  /* Now, the order parameter distribution */
    __targetILP__(iv) {
      phi[iv] = fe->phi->data[addr_rank0(fe->phi->nsites, baseIndex + iv)];
    }

  __targetILP__(iv){
    fe_symm_mu(fe, baseIndex + iv, mu + iv);
    jphi[X*VVL+iv] = 0.0;
    jphi[Y*VVL+iv] = 0.0;
    jphi[Z*VVL+iv] = 0.0;
  }

  for (p = 1; p < NVEL; p++) {
    for (i = 0; i < 3; i++) {
      __targetILP__(iv) {
	jphi[i*VVL+iv] += tc_cv[p][i]* 
	lb->f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, p) ];
      }
    }
  }

  /* Relax order parameters modes. See the comments above. */
  
  for (i = 0; i < 3; i++) {
    for (j = 0; j < 3; j++) {
      __targetILP__(iv) 
	sphi[i][j*VVL+iv] = phi[iv]*u[i*VVL+iv]*u[j*VVL+iv] + mu[iv]*d[i][j];
      /* sphi[i][j] = phi*u[i]*u[j] + cs2*mobility*mu*d_[i][j];*/
    }
    __targetILP__(iv)  jphi[i*VVL+iv] = jphi[i*VVL+iv] 
      - tc_rtau2*(jphi[i*VVL+iv] - phi[iv]*u[i*VVL+iv]);
    /* jphi[i] = phi*u[i];*/
  }
  
  /* Now update the distribution */
  
#ifdef _D3Q19_
  updateDistD3Q19(jdotc,sphidotq,sphi,phi,jphi, lb->f, baseIndex);
#else

  for (p = 0; p < NVEL; p++) {
    
    int dp0 = (p == 0);

    __targetILP__(iv) {
      jdotc[iv]    = 0.0;
      sphidotq[iv] = 0.0;
    }
    
    for (i = 0; i < 3; i++) {
      __targetILP__(iv)  jdotc[iv] += jphi[i*VVL+iv]*tc_cv[p][i];
      for (j = 0; j < 3; j++) {
	__targetILP__(iv)  sphidotq[iv] += sphi[i][j*VVL+iv]*tc_q_[p][i][j];
      }
    }
    
    /* Project all this back to the distributions. The magic
     * here is to move phi into the non-propagating distribution. */
    __targetILP__(iv) 
      lb->f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, p) ] 
      = tc_wv[p]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4)
      + phi[iv]*dp0;
  }
#endif
  
  return;
  
}

__targetEntry__ void lb_collision_binary_lattice(lb_t * lb, 
						 hydro_t * hydro,
						 fe_symm_t * fe,
					       noise_t * noise, int noise_on){
  int baseIndex = 0;

  __targetTLP__(baseIndex, tc_nSites) {
    lb_collision_binary_site(lb, hydro, fe, noise, noise_on, baseIndex);
  }
  
  return;
}

__host__
int lb_collision_binary(lb_t * lb, hydro_t * hydro, map_t * map,
			noise_t * noise, fe_symm_t * fe) {

  int Nall[3];
  int nlocal[3];
  int nSites;
  int nhalo;
  int noise_on = 0;                  /* Fluctuations switch */

  double rtau2;
  double mobility;
  double force_global[3];
  physics_t * phys = NULL;

  const double r2rcs4 = 4.5;         /* The constant 1 / 2 c_s^4 */


  assert (NDIM == 3);
  assert(lb);
  assert(hydro);
  assert(map);

  coords_nlocal(nlocal);
  physics_ref(&phys);
  physics_fbody(phys, force_global);

  noise_present(noise, NOISE_RHO, &noise_on);


  /* The lattice mobility gives tau = (M rho_0 / Delta t) + 1 / 2,
   * or with rho_0 = 1 etc: (1 / tau) = 2 / (2M + 1) */

  physics_mobility(phys, &mobility);
  rtau2 = 2.0 / (1.0 + 2.0*mobility);

  nhalo = coords_nhalo();
  Nall[X] = nlocal[X]+2*nhalo;
  Nall[Y] = nlocal[Y]+2*nhalo;
  Nall[Z] = nlocal[Z]+2*nhalo;
  nSites  = Nall[X]*Nall[Y]*Nall[Z];

  copyConstToTarget(&tc_rtau_shear, &rtau_shear, sizeof(double));
  copyConstToTarget(&tc_rtau_bulk, &rtau_bulk, sizeof(double));
  copyConstToTarget(&tc_r2rcs4, &r2rcs4, sizeof(double));
  copyConstToTarget(tc_wv, wv, NVEL*sizeof(double));
  copyConstToTarget(tc_ma_, ma_, NVEL*NVEL*sizeof(double));
  copyConstToTarget(tc_mi_, mi_, NVEL*NVEL*sizeof(double));
  copyConstToTarget(tc_cv, cv, NVEL*3*sizeof(int));
  copyConstToTarget(&tc_rtau2, &rtau2, sizeof(double));
  copyConstToTarget(&tc_rcs2, &rcs2, sizeof(double));
  copyConstToTarget(&tc_nSites,&nSites, sizeof(int));
  copyConstToTarget(tc_force_global,force_global, 3*sizeof(double));
  copyConstToTarget(tc_q_, q_, NVEL*3*3*sizeof(double));

  checkTargetError("constants");

  TIMER_start(TIMER_COLLIDE_KERNEL);
  lb_collision_binary_lattice __targetLaunch__(nSites) (lb->target, hydro->target, fe->target, noise, noise_on);

  targetSynchronize();
  TIMER_stop(TIMER_COLLIDE_KERNEL);

  return 0;
}

/*****************************************************************************
 *
 *  collision_stats_kt
 *
 *  Reports the equipartition of momentum, and the actual temperature
 *  cf. the expected (input) temperature.
 *
 *****************************************************************************/

 int lb_collision_stats_kt(lb_t * lb, noise_t * noise, map_t * map) {

  int ic, jc, kc, index;
  int nlocal[3];
  int n;
  int status;

  double glocal[4];
  double gtotal[4];
  double rrho;
  double gsite[3];
  double kt;
  physics_t * phys = NULL;

  assert(lb);
  assert(map);
  assert(noise);

  noise_present(noise, NOISE_RHO, &status);
  if (status == 0) return 0;

  physics_ref(&phys);
  physics_kt(phys, &kt);

  coords_nlocal(nlocal);

  glocal[X] = 0.0;
  glocal[Y] = 0.0;
  glocal[Z] = 0.0;
  glocal[3] = 0.0; /* volume of fluid */

  for (ic = 1; ic <= nlocal[X]; ic++) {
    for (jc = 1; jc <= nlocal[Y]; jc++) {
      for (kc = 1; kc <= nlocal[Z]; kc++) {

	index = coords_index(ic, jc, kc);
	map_status(map, index, &status);
	if (status != MAP_FLUID) continue;

	lb_0th_moment(lb, index, LB_RHO, &rrho);
	rrho = 1.0/rrho;
	lb_1st_moment(lb, index, LB_RHO, gsite);

	for (n = 0; n < 3; n++) {
	  glocal[n] += gsite[n]*gsite[n]*rrho;
	}

	glocal[3] += 1.0;

	/* Next cell */
      }
    }
  }

  /* Divide by the actual fluid volume. The reduction is to rank 0 in
   * pe_comm() for output. */

  MPI_Reduce(glocal, gtotal, 4, MPI_DOUBLE, MPI_SUM, 0, pe_comm());

  for (n = 0; n < 3; n++) {
    gtotal[n] /= gtotal[3];
  }

  info("\n");
  info("Isothermal fluctuations\n");
  info("[eqipart.] %14.7e %14.7e %14.7e\n", gtotal[X], gtotal[Y], gtotal[Z]);

  kt *= NDIM;
  info("[measd/kT] %14.7e %14.7e\n", gtotal[X] + gtotal[Y] + gtotal[Z], kt);

  return 0;
}

/*****************************************************************************
 *
 *  lb_collision_ghost_modes_on
 *
 *****************************************************************************/

 __host__ int lb_collision_ghost_modes_on(lb_t * lb) {

   assert(lb);
   assert(lb->p);

  lb->p->isghost = LB_GHOST_ON;

  return 0;
}

/*****************************************************************************
 *
 *  lb_collision_ghost_modes_off
 *
 *****************************************************************************/

 __host__ int lb_collision_ghost_modes_off(lb_t * lb) {

   assert(lb);
   assert(lb->p);

  lb->p->isghost = LB_GHOST_OFF;

  return 0;
}

/*****************************************************************************
 *
 *  lb_collision_relaxation_set
 *
 *****************************************************************************/
 
 __host__ int lb_collision_relaxation_set(lb_t * lb, int nrelax) {

  assert(nrelax == RELAXATION_M10 ||
         nrelax == RELAXATION_BGK ||
         nrelax == RELAXATION_TRT);

  nrelax_ = nrelax;

  return 0;
}

/*****************************************************************************
 *
 *  lb_collision_relaxation_times_set
 *
 *  We have:
 *     Shear viscosity eta = -rho c_s^2 dt ( 1 / lambda + 1 / 2 )
 *
 *  where lambda is the eigenvalue so that the mode relaxation is
 *
 *     m_k^* = m_k + lambda (m_k - m_k^eq)
 *
 *  Bulk viscosity = -rho0 c_s^2 dt (1/3) ( 2 / lambda + 1 )
 *
 *  Note there is an extra normalisation in the lattice fluctuations
 *  which would otherwise give effective kT = cs2
 *
 *****************************************************************************/

__host__ int lb_collision_relaxation_times_set(lb_t * lb, noise_t * noise) {

  int p;
  int noise_on = 0;
  double rho0;
  double kt;
  double eta_shear;
  double eta_bulk;
  double tau, rtau;
  double tau_s;
  double tau_b;
  double tau_g;
  physics_t * phys = NULL;

  assert(lb);
  assert(lb->p);
  assert(noise);

  noise_present(noise, NOISE_RHO, &noise_on);
  physics_ref(&phys);
  physics_rho0(phys, &rho0);

  /* Initialise the relaxation times */
 
  physics_eta_shear(phys, &eta_shear);
  physics_eta_bulk(phys, &eta_bulk);

  rtau_shear = 1.0/(0.5 + eta_shear / (rho0*cs2));
  rtau_bulk  = 1.0/(0.5 + eta_bulk / (rho0*cs2));

  if (nrelax_ == RELAXATION_M10) {
    for (p = NHYDRO; p < NVEL; p++) {
      lb->p->rtau[p] = 1.0;
    }
  }

  if (nrelax_ == RELAXATION_BGK) {
    for (p = 0; p < NVEL; p++) {
      lb->p->rtau[p] = rtau_shear;
    }
  }

  if (nrelax_ == RELAXATION_TRT) {

    assert(NVEL != 9);

    tau  = eta_shear / (rho0*cs2);
    rtau = 0.5 + 2.0*tau/(tau + 3.0/8.0);
    if (rtau > 2.0) rtau = 2.0;

    if (NVEL == 15) {
      lb->p->rtau[10] = rtau_shear;
      lb->p->rtau[11] = rtau;
      lb->p->rtau[12] = rtau;
      lb->p->rtau[13] = rtau;
      lb->p->rtau[14] = rtau_shear;
    }

    if (NVEL == 19) {
      lb->p->rtau[10] = rtau_shear;
      lb->p->rtau[14] = rtau_shear;
      lb->p->rtau[18] = rtau_shear;

      lb->p->rtau[11] = rtau;
      lb->p->rtau[12] = rtau;
      lb->p->rtau[13] = rtau;
      lb->p->rtau[15] = rtau;
      lb->p->rtau[16] = rtau;
      lb->p->rtau[17] = rtau;
    }
  }

  if (noise_on) {

    tau_s = 1.0/rtau_shear;
    tau_b = 1.0/rtau_bulk;

    /* Initialise the stress variances */

    physics_kt(phys, &kt);
    kt = kt*rcs2; /* Without normalisation kT = cs^2 */

    lb->p->var_bulk =
      sqrt(kt)*sqrt(2.0/9.0)*sqrt((tau_b + tau_b - 1.0)/(tau_b*tau_b));
    lb->p->var_shear =
      sqrt(kt)*sqrt(1.0/9.0)*sqrt((tau_s + tau_s - 1.0)/(tau_s*tau_s));

    /* Noise variances */

    for (p = NHYDRO; p < NVEL; p++) {
      tau_g = 1.0/lb->p->rtau[p];
      lb->p->var_noise[p] =
	sqrt(kt/norm_[p])*sqrt((tau_g + tau_g - 1.0)/(tau_g*tau_g));
    }
  }


  if (lb->p->isghost == LB_GHOST_OFF) {
    /* This option is intended to check the M10 without the correct
     * noise terms. Should not be used for a real simulation. */
    /* Eliminate ghost modes and ghost mode noise */
    for (p = NHYDRO; p < NVEL; p++) {
      lb->p->rtau[p] = 1.0;
      lb->p->var_noise[p] = 0.0;
    }
  }

  return 0;
}

/*****************************************************************************
 *
 *  lb_collision_relaxation_times
 *
 *  Return NVEL (inverse) relaxation times. This is really just for
 *  information, so I've put the bulk viscosity of the diagonal of
 *  the stress elements and the shear on the off-diagonal.
 *
 *****************************************************************************/

__host__ int lb_collision_relaxation_times(lb_t * lb, double * tau) {

  int ia, ib;
  int mode;

  assert(nrelax_ == RELAXATION_M10);

  /* Density and momentum (modes 0, 1, .. NDIM) */

  for (ia = 0; ia <= NDIM; ia++) {
    tau[ia] = 0.0;
  }

  /* Stress */

  mode = 0;
  for (ia = 0; ia < NDIM; ia++) {
    for (ib = ia; ib < NDIM; ib++) {
      if (ia == ib) tau[1 + NDIM + mode++] = rtau_shear;
      if (ia != ib) tau[1 + NDIM + mode++] = rtau_bulk;
    }
  }

  for (ia = 1; ia < NDIM; ia++) {
    for (ib = 0; ib < ia; ib++) {
      if (ia == ib) tau[1 + NDIM + mode++] = rtau_shear;
      if (ia != ib) tau[1 + NDIM + mode++] = rtau_bulk;
    }
  }

  /* Ghosts */

  for (ia = NHYDRO; ia < NVEL; ia++) {
    tau[ia] = lb->p->rtau[ia];
  }

  return 0;
}

/*****************************************************************************
 *
 *  lb_collision_fluctuations
 *
 *  Compute that fluctuating contributions to the distribution at
 *  the current lattice site index.
 *
 *  There are NDIM*(NDIM+1)/2 independent stress modes, and
 *  NVEL - NHYDRO ghost modes.
 *
 *****************************************************************************/

static __host__ __device__ __inline__
  void lb_collision_fluctuations(lb_t * lb, noise_t * noise, int index,
				 double shat[3][3], double ghat[NVEL]) {
  int ia;
  double tr;
  double random[NNOISE_MAX];

  assert(lb);
  assert(lb->p);
  assert(noise);
  assert(NNOISE_MAX >= NDIM*(NDIM+1)/2);
  assert(NNOISE_MAX >= (NVEL - NHYDRO));
  assert(NDIM == 2 || NDIM == 3);

  /* Set symetric random stress matrix (elements with unit variance);
   * in practice always 3d (= 6 elements) required */

  noise_reap_n(noise, index, 6, random);

  shat[X][X] = random[0];
  shat[X][Y] = random[1];
  shat[X][Z] = random[2];

  shat[Y][X] = shat[X][Y];
  shat[Y][Y] = random[3];
  shat[Y][Z] = random[4];

  shat[Z][X] = shat[X][Z];
  shat[Z][Y] = shat[Y][Z];
  shat[Z][Z] = random[5];

  /* Compute the trace and the traceless part */

  tr = (1.0/NDIM)*(shat[X][X] + shat[Y][Y] + (NDIM - 2.0)*shat[Z][Z]);
  shat[X][X] -= tr;
  shat[Y][Y] -= tr;
  shat[Z][Z] -= tr;

  /* Set variance of the traceless part */

  shat[X][X] *= lb->p->var_shear*sqrt(2.0);
  shat[X][Y] *= lb->p->var_shear;
  shat[X][Z] *= lb->p->var_shear;

  shat[Y][X] *= lb->p->var_shear;
  shat[Y][Y] *= lb->p->var_shear*sqrt(2.0);
  shat[Y][Z] *= lb->p->var_shear;

  shat[Z][X] *= lb->p->var_shear;
  shat[Z][Y] *= lb->p->var_shear;
  shat[Z][Z] *= lb->p->var_shear*sqrt(2.0);

  /* Set variance of trace and recombine... */

  tr *= (lb->p->var_bulk);

  shat[X][X] += tr;
  shat[Y][Y] += tr;
  shat[Z][Z] += tr;

  /* Ghost modes */

  for (ia = 0; ia < NHYDRO; ia++) {
    ghat[ia] = 0.0;
  }

  if (lb->p->isghost == LB_GHOST_ON) {
    noise_reap_n(noise, index, NVEL-NHYDRO, random);

    for (ia = NHYDRO; ia < NVEL; ia++) {
      ghat[ia] = lb->p->var_noise[ia]*random[ia - NHYDRO];
    }
  }

  return;
}


/*  below are specialized fast unrolled version of the d3q19 19x19 matrix 
 *  multiplications. These are significantly faster because there is 
 * duplication in the 19x19 matrix. Explicitly coding the values means 
 * less data movement on the chip. */

#define w0 (12.0/36.0)
#define w1  (2.0/36.0)
#define w2  (1.0/36.0)

#define c0        0.0
#define c1        1.0
#define c2        2.0
#define r3   (1.0/3.0)
#define r6   (1.0/6.0)
#define t3   (2.0/3.0)
#define r2   (1.0/2.0)
#define r4   (1.0/4.0)
#define r8   (1.0/8.0)

#define wc ( 1.0/72.0)
#define wb ( 3.0/72.0)
#define wa ( 6.0/72.0)
#define t4 (16.0/72.0)
#define wd ( 1.0/48.0)
#define we ( 3.0/48.0)


 __target__ void d3q19matmult(double* mode, const double* __restrict__ ftmp_d, int ndist, int baseIndex)
{

  int m, il=0;

   for (m = 0; m < NVEL; m++) { 
       __targetILP__(il) mode[m*VVL+il] = 0.0; 
   }


  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*c1;
  __targetILP__(il) mode[0*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*c1;

  /* m=1*/
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c0;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*c1;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*c1;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*c1;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*c1;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*c1;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c0;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*c0;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*c0;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*c0;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*c0;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*c0;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*c0;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*c0;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*-c1;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*-c1;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*-c1;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*-c1;
  __targetILP__(il) mode[1*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*-c1;

  /* m=2 */
  __targetILP__(il) mode[2*VVL+il]=0.;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c0;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*c1;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*c0;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*c0;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*c0;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*-c1;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c1;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*c1;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*c1;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*c0;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*c0;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*-c1;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*-c1;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*-c1;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*c1;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*c0;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*c0;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*c0;
  __targetILP__(il) mode[2*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*-c1;

  /* m=3*/
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c0;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*c0;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*c1;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*c0;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*-c1;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*c0;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c1;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*c0;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*-c1;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*c1;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*-c1;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*c1;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*c0;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*-c1;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*c0;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*c1;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*c0;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*-c1;
  __targetILP__(il) mode[3*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*c0;

  /* m=4*/
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*-r3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*t3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*t3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*t3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*t3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*t3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*-r3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*-r3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*-r3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*-r3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*-r3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*-r3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*-r3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*-r3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*t3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*t3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*t3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*t3;
  __targetILP__(il) mode[4*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*t3;

  /* m=5 */
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c0;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*c1;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*c0;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*c0;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*c0;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*-c1;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c0;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*c0;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*c0;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*c0;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*c0;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*c0;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*c0;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*c0;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*-c1;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*c0;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*c0;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*c0;
  __targetILP__(il) mode[5*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*c1;

  /* m=6*/
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c0;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*c0;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*c1;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*c0;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*-c1;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*c0;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c0;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*c0;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*c0;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*c0;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*c0;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*c0;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*c0;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*c0;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*c0;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*-c1;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*c0;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*c1;
  __targetILP__(il) mode[6*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*c0;

  /* m=7*/
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*-r3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*t3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*-r3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*-r3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*-r3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*t3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*t3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*t3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*t3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*-r3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*-r3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*t3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*t3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*t3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*t3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*-r3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*-r3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*-r3;
  __targetILP__(il) mode[7*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*t3;

  /* m=8*/
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c0;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*c0;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*c0;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*c0;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*c0;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*c0;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c1;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*c0;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*-c1;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*c0;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*c0;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*-c1;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*c0;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*c1;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*c0;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*c0;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*c0;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*c0;
  __targetILP__(il) mode[8*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*c0;

  /* m=9*/
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*-r3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*-r3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*t3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*-r3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*t3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*-r3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*t3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*-r3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*t3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*t3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*t3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*t3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*-r3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*t3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*-r3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*t3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*-r3;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*t3 ;
  __targetILP__(il) mode[9*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*-r3;

  /* m=10*/
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c0;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*-c2;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*c1;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*c1;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*c1;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*-c2;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c1;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*c1;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*c1;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*-c2;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*-c2;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*c1;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*c1;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*c1;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*-c2;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*c1;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*c1;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*c1;
  __targetILP__(il) mode[10*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*-c2;

  /* m=11*/
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c0;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*-c2;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*c1;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*c1;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*c1;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*-c2;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c0;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*c0;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*c0;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*c0;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*c0;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*c0;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*c0;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*c0;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*c2;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*-c1;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*-c1;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*-c1;
  __targetILP__(il) mode[11*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*c2;

  /* m=12 */
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c0;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*-c2;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*c0;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*c0;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*c0;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*c2;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c1;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*c1;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*c1;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*c0;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*c0;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*-c1;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*-c1;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*-c1;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*-c2;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*c0;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*c0;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*c0;
  __targetILP__(il) mode[12*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*c2;

  /* m=13 */
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c0;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*c0;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*c1;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*c0;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*c0;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*c0;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c1;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*c0;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*-c1;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*-c2;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*c2;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*c1;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*c0;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*-c1;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*c0;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*c1;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*c0;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*-c1;
  __targetILP__(il) mode[13*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*c0;

  /* m=14*/
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c0;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*c0;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*-c1;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*c1;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*-c1;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*c0;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c1;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*-c1;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*c1;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*c0;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*c0;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*c1;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*-c1;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*c1;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*c0;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*-c1;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*c1;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*-c1;
  __targetILP__(il) mode[14*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*c0;

  /* m=15*/
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c0;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*c0;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*-c1;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*c1;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*-c1;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*c0;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c0;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*c0;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*c0;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*c0;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*c0;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*c0;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*c0;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*c0;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*c0;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*c1;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*-c1;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*c1;
  __targetILP__(il) mode[15*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*c0;

  /* m=16*/
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c0;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*c0;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*c0;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*c0;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*c0;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*c0;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c1;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*-c1;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*c1;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*c0;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*c0;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*-c1;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*c1;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*-c1;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*c0;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*c0;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*c0;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*c0;
  __targetILP__(il) mode[16*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*c0;

  /* m=17*/
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c0;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*c0;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*-c1;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*c0;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*c1;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*c0;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c1;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*c0;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*-c1;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*c0;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*c0;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*c1;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*c0;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*-c1;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*c0;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*-c1;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*c0;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*c1;
  __targetILP__(il) mode[17*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*c0;

  /* m=18*/
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)]*c1;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)]*c1;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)]*c1;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)]*-c2;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)]*c1;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)]*c1;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)]*c1;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)]*-c2;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)]*c1;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)]*-c2;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)]*-c2;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)]*c1;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)]*-c2;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)]*c1;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)]*c1;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)]*c1;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)]*-c2;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)]*c1;
  __targetILP__(il) mode[18*VVL+il] += ftmp_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)]*c1;
 
}


 __target__ void d3q19matmult2(double* mode, double* f_d, int ndist, int baseIndex)
{

  double ftmp[VVL];

  int il=0;

  
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w0*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -r2*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -r2*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -r2*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[18*VVL+il];
  __targetILP__(il)   f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 0)] = ftmp[il];
  
  /* p=1*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 1)] = ftmp[il];
  
  /* p=2*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += -we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 2)] =  ftmp[il];
  
  /* p=3*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w1*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += -w1*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 3)] =  ftmp[il];
  
  /* p=4*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += -we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 4)] = ftmp[il];
  
  /* p=5*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 5)] = ftmp[il];
  
  /*p=6 */
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 6)] =  ftmp[il];
  
  /* p=7*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w1*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += -w1*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 7)] = ftmp[il];
  
  /* p=8*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 8)] =  ftmp[il];
  
  /* p=9*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w1*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += -r6*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += -w1*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 9)] = ftmp[il];
  
  /* p=10*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w1*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += -r6*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += -w1*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 10)] = ftmp[il];
  
  /* p=11*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 11)] =  ftmp[il];
  
  /* p=12*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w1*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += -r6*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += -w1*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 12)] = ftmp[il];
  
  /* p=13*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 13)] =  ftmp[il];
  
  /* p=14*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 14)] =  ftmp[il];
  
  /* p=15*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += -we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 15)] =  ftmp[il];
  
  /* p=16*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w1*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += -r6*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += -w1*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 16)] =ftmp[il];
  
  /* p=17*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += -we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 17)] = ftmp[il];
  
  /* p=18*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) f_d[LB_ADDR(tc_nSites, ndist, NVEL, baseIndex + il,0, 18)] = ftmp[il];


}

__target__ void d3q19matmultchunk(double* mode, const double* __restrict__ fchunk, int baseIndex)
{

  int m, il=0;

   for (m = 0; m < NVEL; m++) { 
       __targetILP__(il) mode[m*VVL+il] = 0.0; 
   }


  __targetILP__(il) mode[0*VVL+il] += fchunk[0*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[1*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[2*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[3*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[4*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[5*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[6*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[7*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[8*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[9*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[10*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[11*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[12*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[13*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[14*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[15*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[16*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[17*VVL+il]*c1;
  __targetILP__(il) mode[0*VVL+il] += fchunk[18*VVL+il]*c1;

  /* m=1*/
  __targetILP__(il) mode[1*VVL+il] += fchunk[0*VVL+il]*c0;
  __targetILP__(il) mode[1*VVL+il] += fchunk[1*VVL+il]*c1;
  __targetILP__(il) mode[1*VVL+il] += fchunk[2*VVL+il]*c1;
  __targetILP__(il) mode[1*VVL+il] += fchunk[3*VVL+il]*c1;
  __targetILP__(il) mode[1*VVL+il] += fchunk[4*VVL+il]*c1;
  __targetILP__(il) mode[1*VVL+il] += fchunk[5*VVL+il]*c1;
  __targetILP__(il) mode[1*VVL+il] += fchunk[6*VVL+il]*c0;
  __targetILP__(il) mode[1*VVL+il] += fchunk[7*VVL+il]*c0;
  __targetILP__(il) mode[1*VVL+il] += fchunk[8*VVL+il]*c0;
  __targetILP__(il) mode[1*VVL+il] += fchunk[9*VVL+il]*c0;
  __targetILP__(il) mode[1*VVL+il] += fchunk[10*VVL+il]*c0;
  __targetILP__(il) mode[1*VVL+il] += fchunk[11*VVL+il]*c0;
  __targetILP__(il) mode[1*VVL+il] += fchunk[12*VVL+il]*c0;
  __targetILP__(il) mode[1*VVL+il] += fchunk[13*VVL+il]*c0;
  __targetILP__(il) mode[1*VVL+il] += fchunk[14*VVL+il]*-c1;
  __targetILP__(il) mode[1*VVL+il] += fchunk[15*VVL+il]*-c1;
  __targetILP__(il) mode[1*VVL+il] += fchunk[16*VVL+il]*-c1;
  __targetILP__(il) mode[1*VVL+il] += fchunk[17*VVL+il]*-c1;
  __targetILP__(il) mode[1*VVL+il] += fchunk[18*VVL+il]*-c1;

  /* m=2*/
  __targetILP__(il) mode[2*VVL+il]=0.;
  __targetILP__(il) mode[2*VVL+il] += fchunk[0*VVL+il]*c0;
  __targetILP__(il) mode[2*VVL+il] += fchunk[1*VVL+il]*c1;
  __targetILP__(il) mode[2*VVL+il] += fchunk[2*VVL+il]*c0;
  __targetILP__(il) mode[2*VVL+il] += fchunk[3*VVL+il]*c0;
  __targetILP__(il) mode[2*VVL+il] += fchunk[4*VVL+il]*c0;
  __targetILP__(il) mode[2*VVL+il] += fchunk[5*VVL+il]*-c1;
  __targetILP__(il) mode[2*VVL+il] += fchunk[6*VVL+il]*c1;
  __targetILP__(il) mode[2*VVL+il] += fchunk[7*VVL+il]*c1;
  __targetILP__(il) mode[2*VVL+il] += fchunk[8*VVL+il]*c1;
  __targetILP__(il) mode[2*VVL+il] += fchunk[9*VVL+il]*c0;
  __targetILP__(il) mode[2*VVL+il] += fchunk[10*VVL+il]*c0;
  __targetILP__(il) mode[2*VVL+il] += fchunk[11*VVL+il]*-c1;
  __targetILP__(il) mode[2*VVL+il] += fchunk[12*VVL+il]*-c1;
  __targetILP__(il) mode[2*VVL+il] += fchunk[13*VVL+il]*-c1;
  __targetILP__(il) mode[2*VVL+il] += fchunk[14*VVL+il]*c1;
  __targetILP__(il) mode[2*VVL+il] += fchunk[15*VVL+il]*c0;
  __targetILP__(il) mode[2*VVL+il] += fchunk[16*VVL+il]*c0;
  __targetILP__(il) mode[2*VVL+il] += fchunk[17*VVL+il]*c0;
  __targetILP__(il) mode[2*VVL+il] += fchunk[18*VVL+il]*-c1;

  /* m=3*/
  __targetILP__(il) mode[3*VVL+il] += fchunk[0*VVL+il]*c0;
  __targetILP__(il) mode[3*VVL+il] += fchunk[1*VVL+il]*c0;
  __targetILP__(il) mode[3*VVL+il] += fchunk[2*VVL+il]*c1;
  __targetILP__(il) mode[3*VVL+il] += fchunk[3*VVL+il]*c0;
  __targetILP__(il) mode[3*VVL+il] += fchunk[4*VVL+il]*-c1;
  __targetILP__(il) mode[3*VVL+il] += fchunk[5*VVL+il]*c0;
  __targetILP__(il) mode[3*VVL+il] += fchunk[6*VVL+il]*c1;
  __targetILP__(il) mode[3*VVL+il] += fchunk[7*VVL+il]*c0;
  __targetILP__(il) mode[3*VVL+il] += fchunk[8*VVL+il]*-c1;
  __targetILP__(il) mode[3*VVL+il] += fchunk[9*VVL+il]*c1;
  __targetILP__(il) mode[3*VVL+il] += fchunk[10*VVL+il]*-c1;
  __targetILP__(il) mode[3*VVL+il] += fchunk[11*VVL+il]*c1;
  __targetILP__(il) mode[3*VVL+il] += fchunk[12*VVL+il]*c0;
  __targetILP__(il) mode[3*VVL+il] += fchunk[13*VVL+il]*-c1;
  __targetILP__(il) mode[3*VVL+il] += fchunk[14*VVL+il]*c0;
  __targetILP__(il) mode[3*VVL+il] += fchunk[15*VVL+il]*c1;
  __targetILP__(il) mode[3*VVL+il] += fchunk[16*VVL+il]*c0;
  __targetILP__(il) mode[3*VVL+il] += fchunk[17*VVL+il]*-c1;
  __targetILP__(il) mode[3*VVL+il] += fchunk[18*VVL+il]*c0;

  /* m=4*/
  __targetILP__(il) mode[4*VVL+il] += fchunk[0*VVL+il]*-r3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[1*VVL+il]*t3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[2*VVL+il]*t3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[3*VVL+il]*t3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[4*VVL+il]*t3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[5*VVL+il]*t3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[6*VVL+il]*-r3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[7*VVL+il]*-r3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[8*VVL+il]*-r3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[9*VVL+il]*-r3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[10*VVL+il]*-r3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[11*VVL+il]*-r3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[12*VVL+il]*-r3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[13*VVL+il]*-r3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[14*VVL+il]*t3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[15*VVL+il]*t3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[16*VVL+il]*t3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[17*VVL+il]*t3;
  __targetILP__(il) mode[4*VVL+il] += fchunk[18*VVL+il]*t3;

  /* m=5*/
  __targetILP__(il) mode[5*VVL+il] += fchunk[0*VVL+il]*c0;
  __targetILP__(il) mode[5*VVL+il] += fchunk[1*VVL+il]*c1;
  __targetILP__(il) mode[5*VVL+il] += fchunk[2*VVL+il]*c0;
  __targetILP__(il) mode[5*VVL+il] += fchunk[3*VVL+il]*c0;
  __targetILP__(il) mode[5*VVL+il] += fchunk[4*VVL+il]*c0;
  __targetILP__(il) mode[5*VVL+il] += fchunk[5*VVL+il]*-c1;
  __targetILP__(il) mode[5*VVL+il] += fchunk[6*VVL+il]*c0;
  __targetILP__(il) mode[5*VVL+il] += fchunk[7*VVL+il]*c0;
  __targetILP__(il) mode[5*VVL+il] += fchunk[8*VVL+il]*c0;
  __targetILP__(il) mode[5*VVL+il] += fchunk[9*VVL+il]*c0;
  __targetILP__(il) mode[5*VVL+il] += fchunk[10*VVL+il]*c0;
  __targetILP__(il) mode[5*VVL+il] += fchunk[11*VVL+il]*c0;
  __targetILP__(il) mode[5*VVL+il] += fchunk[12*VVL+il]*c0;
  __targetILP__(il) mode[5*VVL+il] += fchunk[13*VVL+il]*c0;
  __targetILP__(il) mode[5*VVL+il] += fchunk[14*VVL+il]*-c1;
  __targetILP__(il) mode[5*VVL+il] += fchunk[15*VVL+il]*c0;
  __targetILP__(il) mode[5*VVL+il] += fchunk[16*VVL+il]*c0;
  __targetILP__(il) mode[5*VVL+il] += fchunk[17*VVL+il]*c0;
  __targetILP__(il) mode[5*VVL+il] += fchunk[18*VVL+il]*c1;

  /* m=6*/
  __targetILP__(il) mode[6*VVL+il] += fchunk[0*VVL+il]*c0;
  __targetILP__(il) mode[6*VVL+il] += fchunk[1*VVL+il]*c0;
  __targetILP__(il) mode[6*VVL+il] += fchunk[2*VVL+il]*c1;
  __targetILP__(il) mode[6*VVL+il] += fchunk[3*VVL+il]*c0;
  __targetILP__(il) mode[6*VVL+il] += fchunk[4*VVL+il]*-c1;
  __targetILP__(il) mode[6*VVL+il] += fchunk[5*VVL+il]*c0;
  __targetILP__(il) mode[6*VVL+il] += fchunk[6*VVL+il]*c0;
  __targetILP__(il) mode[6*VVL+il] += fchunk[7*VVL+il]*c0;
  __targetILP__(il) mode[6*VVL+il] += fchunk[8*VVL+il]*c0;
  __targetILP__(il) mode[6*VVL+il] += fchunk[9*VVL+il]*c0;
  __targetILP__(il) mode[6*VVL+il] += fchunk[10*VVL+il]*c0;
  __targetILP__(il) mode[6*VVL+il] += fchunk[11*VVL+il]*c0;
  __targetILP__(il) mode[6*VVL+il] += fchunk[12*VVL+il]*c0;
  __targetILP__(il) mode[6*VVL+il] += fchunk[13*VVL+il]*c0;
  __targetILP__(il) mode[6*VVL+il] += fchunk[14*VVL+il]*c0;
  __targetILP__(il) mode[6*VVL+il] += fchunk[15*VVL+il]*-c1;
  __targetILP__(il) mode[6*VVL+il] += fchunk[16*VVL+il]*c0;
  __targetILP__(il) mode[6*VVL+il] += fchunk[17*VVL+il]*c1;
  __targetILP__(il) mode[6*VVL+il] += fchunk[18*VVL+il]*c0;

  /* m=7*/
  __targetILP__(il) mode[7*VVL+il] += fchunk[0*VVL+il]*-r3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[1*VVL+il]*t3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[2*VVL+il]*-r3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[3*VVL+il]*-r3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[4*VVL+il]*-r3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[5*VVL+il]*t3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[6*VVL+il]*t3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[7*VVL+il]*t3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[8*VVL+il]*t3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[9*VVL+il]*-r3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[10*VVL+il]*-r3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[11*VVL+il]*t3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[12*VVL+il]*t3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[13*VVL+il]*t3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[14*VVL+il]*t3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[15*VVL+il]*-r3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[16*VVL+il]*-r3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[17*VVL+il]*-r3;
  __targetILP__(il) mode[7*VVL+il] += fchunk[18*VVL+il]*t3;

  /* m=8*/
  __targetILP__(il) mode[8*VVL+il] += fchunk[0*VVL+il]*c0;
  __targetILP__(il) mode[8*VVL+il] += fchunk[1*VVL+il]*c0;
  __targetILP__(il) mode[8*VVL+il] += fchunk[2*VVL+il]*c0;
  __targetILP__(il) mode[8*VVL+il] += fchunk[3*VVL+il]*c0;
  __targetILP__(il) mode[8*VVL+il] += fchunk[4*VVL+il]*c0;
  __targetILP__(il) mode[8*VVL+il] += fchunk[5*VVL+il]*c0;
  __targetILP__(il) mode[8*VVL+il] += fchunk[6*VVL+il]*c1;
  __targetILP__(il) mode[8*VVL+il] += fchunk[7*VVL+il]*c0;
  __targetILP__(il) mode[8*VVL+il] += fchunk[8*VVL+il]*-c1;
  __targetILP__(il) mode[8*VVL+il] += fchunk[9*VVL+il]*c0;
  __targetILP__(il) mode[8*VVL+il] += fchunk[10*VVL+il]*c0;
  __targetILP__(il) mode[8*VVL+il] += fchunk[11*VVL+il]*-c1;
  __targetILP__(il) mode[8*VVL+il] += fchunk[12*VVL+il]*c0;
  __targetILP__(il) mode[8*VVL+il] += fchunk[13*VVL+il]*c1;
  __targetILP__(il) mode[8*VVL+il] += fchunk[14*VVL+il]*c0;
  __targetILP__(il) mode[8*VVL+il] += fchunk[15*VVL+il]*c0;
  __targetILP__(il) mode[8*VVL+il] += fchunk[16*VVL+il]*c0;
  __targetILP__(il) mode[8*VVL+il] += fchunk[17*VVL+il]*c0;
  __targetILP__(il) mode[8*VVL+il] += fchunk[18*VVL+il]*c0;

  /* m=9*/
  __targetILP__(il) mode[9*VVL+il] += fchunk[0*VVL+il]*-r3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[1*VVL+il]*-r3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[2*VVL+il]*t3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[3*VVL+il]*-r3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[4*VVL+il]*t3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[5*VVL+il]*-r3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[6*VVL+il]*t3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[7*VVL+il]*-r3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[8*VVL+il]*t3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[9*VVL+il]*t3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[10*VVL+il]*t3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[11*VVL+il]*t3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[12*VVL+il]*-r3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[13*VVL+il]*t3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[14*VVL+il]*-r3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[15*VVL+il]*t3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[16*VVL+il]*-r3;
  __targetILP__(il) mode[9*VVL+il] += fchunk[17*VVL+il]*t3 ;
  __targetILP__(il) mode[9*VVL+il] += fchunk[18*VVL+il]*-r3;

  /* m=10*/
  __targetILP__(il) mode[10*VVL+il] += fchunk[0*VVL+il]*c0;
  __targetILP__(il) mode[10*VVL+il] += fchunk[1*VVL+il]*-c2;
  __targetILP__(il) mode[10*VVL+il] += fchunk[2*VVL+il]*c1;
  __targetILP__(il) mode[10*VVL+il] += fchunk[3*VVL+il]*c1;
  __targetILP__(il) mode[10*VVL+il] += fchunk[4*VVL+il]*c1;
  __targetILP__(il) mode[10*VVL+il] += fchunk[5*VVL+il]*-c2;
  __targetILP__(il) mode[10*VVL+il] += fchunk[6*VVL+il]*c1;
  __targetILP__(il) mode[10*VVL+il] += fchunk[7*VVL+il]*c1;
  __targetILP__(il) mode[10*VVL+il] += fchunk[8*VVL+il]*c1;
  __targetILP__(il) mode[10*VVL+il] += fchunk[9*VVL+il]*-c2;
  __targetILP__(il) mode[10*VVL+il] += fchunk[10*VVL+il]*-c2;
  __targetILP__(il) mode[10*VVL+il] += fchunk[11*VVL+il]*c1;
  __targetILP__(il) mode[10*VVL+il] += fchunk[12*VVL+il]*c1;
  __targetILP__(il) mode[10*VVL+il] += fchunk[13*VVL+il]*c1;
  __targetILP__(il) mode[10*VVL+il] += fchunk[14*VVL+il]*-c2;
  __targetILP__(il) mode[10*VVL+il] += fchunk[15*VVL+il]*c1;
  __targetILP__(il) mode[10*VVL+il] += fchunk[16*VVL+il]*c1;
  __targetILP__(il) mode[10*VVL+il] += fchunk[17*VVL+il]*c1;
  __targetILP__(il) mode[10*VVL+il] += fchunk[18*VVL+il]*-c2;

  /* m=11*/
  __targetILP__(il) mode[11*VVL+il] += fchunk[0*VVL+il]*c0;
  __targetILP__(il) mode[11*VVL+il] += fchunk[1*VVL+il]*-c2;
  __targetILP__(il) mode[11*VVL+il] += fchunk[2*VVL+il]*c1;
  __targetILP__(il) mode[11*VVL+il] += fchunk[3*VVL+il]*c1;
  __targetILP__(il) mode[11*VVL+il] += fchunk[4*VVL+il]*c1;
  __targetILP__(il) mode[11*VVL+il] += fchunk[5*VVL+il]*-c2;
  __targetILP__(il) mode[11*VVL+il] += fchunk[6*VVL+il]*c0;
  __targetILP__(il) mode[11*VVL+il] += fchunk[7*VVL+il]*c0;
  __targetILP__(il) mode[11*VVL+il] += fchunk[8*VVL+il]*c0;
  __targetILP__(il) mode[11*VVL+il] += fchunk[9*VVL+il]*c0;
  __targetILP__(il) mode[11*VVL+il] += fchunk[10*VVL+il]*c0;
  __targetILP__(il) mode[11*VVL+il] += fchunk[11*VVL+il]*c0;
  __targetILP__(il) mode[11*VVL+il] += fchunk[12*VVL+il]*c0;
  __targetILP__(il) mode[11*VVL+il] += fchunk[13*VVL+il]*c0;
  __targetILP__(il) mode[11*VVL+il] += fchunk[14*VVL+il]*c2;
  __targetILP__(il) mode[11*VVL+il] += fchunk[15*VVL+il]*-c1;
  __targetILP__(il) mode[11*VVL+il] += fchunk[16*VVL+il]*-c1;
  __targetILP__(il) mode[11*VVL+il] += fchunk[17*VVL+il]*-c1;
  __targetILP__(il) mode[11*VVL+il] += fchunk[18*VVL+il]*c2;

  /* m=12*/
  __targetILP__(il) mode[12*VVL+il] += fchunk[0*VVL+il]*c0;
  __targetILP__(il) mode[12*VVL+il] += fchunk[1*VVL+il]*-c2;
  __targetILP__(il) mode[12*VVL+il] += fchunk[2*VVL+il]*c0;
  __targetILP__(il) mode[12*VVL+il] += fchunk[3*VVL+il]*c0;
  __targetILP__(il) mode[12*VVL+il] += fchunk[4*VVL+il]*c0;
  __targetILP__(il) mode[12*VVL+il] += fchunk[5*VVL+il]*c2;
  __targetILP__(il) mode[12*VVL+il] += fchunk[6*VVL+il]*c1;
  __targetILP__(il) mode[12*VVL+il] += fchunk[7*VVL+il]*c1;
  __targetILP__(il) mode[12*VVL+il] += fchunk[8*VVL+il]*c1;
  __targetILP__(il) mode[12*VVL+il] += fchunk[9*VVL+il]*c0;
  __targetILP__(il) mode[12*VVL+il] += fchunk[10*VVL+il]*c0;
  __targetILP__(il) mode[12*VVL+il] += fchunk[11*VVL+il]*-c1;
  __targetILP__(il) mode[12*VVL+il] += fchunk[12*VVL+il]*-c1;
  __targetILP__(il) mode[12*VVL+il] += fchunk[13*VVL+il]*-c1;
  __targetILP__(il) mode[12*VVL+il] += fchunk[14*VVL+il]*-c2;
  __targetILP__(il) mode[12*VVL+il] += fchunk[15*VVL+il]*c0;
  __targetILP__(il) mode[12*VVL+il] += fchunk[16*VVL+il]*c0;
  __targetILP__(il) mode[12*VVL+il] += fchunk[17*VVL+il]*c0;
  __targetILP__(il) mode[12*VVL+il] += fchunk[18*VVL+il]*c2;

  /* m=13*/
  __targetILP__(il) mode[13*VVL+il] += fchunk[0*VVL+il]*c0;
  __targetILP__(il) mode[13*VVL+il] += fchunk[1*VVL+il]*c0;
  __targetILP__(il) mode[13*VVL+il] += fchunk[2*VVL+il]*c1;
  __targetILP__(il) mode[13*VVL+il] += fchunk[3*VVL+il]*c0;
  __targetILP__(il) mode[13*VVL+il] += fchunk[4*VVL+il]*c0;
  __targetILP__(il) mode[13*VVL+il] += fchunk[5*VVL+il]*c0;
  __targetILP__(il) mode[13*VVL+il] += fchunk[6*VVL+il]*c1;
  __targetILP__(il) mode[13*VVL+il] += fchunk[7*VVL+il]*c0;
  __targetILP__(il) mode[13*VVL+il] += fchunk[8*VVL+il]*-c1;
  __targetILP__(il) mode[13*VVL+il] += fchunk[9*VVL+il]*-c2;
  __targetILP__(il) mode[13*VVL+il] += fchunk[10*VVL+il]*c2;
  __targetILP__(il) mode[13*VVL+il] += fchunk[11*VVL+il]*c1;
  __targetILP__(il) mode[13*VVL+il] += fchunk[12*VVL+il]*c0;
  __targetILP__(il) mode[13*VVL+il] += fchunk[13*VVL+il]*-c1;
  __targetILP__(il) mode[13*VVL+il] += fchunk[14*VVL+il]*c0;
  __targetILP__(il) mode[13*VVL+il] += fchunk[15*VVL+il]*c1;
  __targetILP__(il) mode[13*VVL+il] += fchunk[16*VVL+il]*c0;
  __targetILP__(il) mode[13*VVL+il] += fchunk[17*VVL+il]*-c1;
  __targetILP__(il) mode[13*VVL+il] += fchunk[18*VVL+il]*c0;

  /* m=14*/
  __targetILP__(il) mode[14*VVL+il] += fchunk[0*VVL+il]*c0;
  __targetILP__(il) mode[14*VVL+il] += fchunk[1*VVL+il]*c0;
  __targetILP__(il) mode[14*VVL+il] += fchunk[2*VVL+il]*-c1;
  __targetILP__(il) mode[14*VVL+il] += fchunk[3*VVL+il]*c1;
  __targetILP__(il) mode[14*VVL+il] += fchunk[4*VVL+il]*-c1;
  __targetILP__(il) mode[14*VVL+il] += fchunk[5*VVL+il]*c0;
  __targetILP__(il) mode[14*VVL+il] += fchunk[6*VVL+il]*c1;
  __targetILP__(il) mode[14*VVL+il] += fchunk[7*VVL+il]*-c1;
  __targetILP__(il) mode[14*VVL+il] += fchunk[8*VVL+il]*c1;
  __targetILP__(il) mode[14*VVL+il] += fchunk[9*VVL+il]*c0;
  __targetILP__(il) mode[14*VVL+il] += fchunk[10*VVL+il]*c0;
  __targetILP__(il) mode[14*VVL+il] += fchunk[11*VVL+il]*c1;
  __targetILP__(il) mode[14*VVL+il] += fchunk[12*VVL+il]*-c1;
  __targetILP__(il) mode[14*VVL+il] += fchunk[13*VVL+il]*c1;
  __targetILP__(il) mode[14*VVL+il] += fchunk[14*VVL+il]*c0;
  __targetILP__(il) mode[14*VVL+il] += fchunk[15*VVL+il]*-c1;
  __targetILP__(il) mode[14*VVL+il] += fchunk[16*VVL+il]*c1;
  __targetILP__(il) mode[14*VVL+il] += fchunk[17*VVL+il]*-c1;
  __targetILP__(il) mode[14*VVL+il] += fchunk[18*VVL+il]*c0;

  /* m=15*/
  __targetILP__(il) mode[15*VVL+il] += fchunk[0*VVL+il]*c0;
  __targetILP__(il) mode[15*VVL+il] += fchunk[1*VVL+il]*c0;
  __targetILP__(il) mode[15*VVL+il] += fchunk[2*VVL+il]*-c1;
  __targetILP__(il) mode[15*VVL+il] += fchunk[3*VVL+il]*c1;
  __targetILP__(il) mode[15*VVL+il] += fchunk[4*VVL+il]*-c1;
  __targetILP__(il) mode[15*VVL+il] += fchunk[5*VVL+il]*c0;
  __targetILP__(il) mode[15*VVL+il] += fchunk[6*VVL+il]*c0;
  __targetILP__(il) mode[15*VVL+il] += fchunk[7*VVL+il]*c0;
  __targetILP__(il) mode[15*VVL+il] += fchunk[8*VVL+il]*c0;
  __targetILP__(il) mode[15*VVL+il] += fchunk[9*VVL+il]*c0;
  __targetILP__(il) mode[15*VVL+il] += fchunk[10*VVL+il]*c0;
  __targetILP__(il) mode[15*VVL+il] += fchunk[11*VVL+il]*c0;
  __targetILP__(il) mode[15*VVL+il] += fchunk[12*VVL+il]*c0;
  __targetILP__(il) mode[15*VVL+il] += fchunk[13*VVL+il]*c0;
  __targetILP__(il) mode[15*VVL+il] += fchunk[14*VVL+il]*c0;
  __targetILP__(il) mode[15*VVL+il] += fchunk[15*VVL+il]*c1;
  __targetILP__(il) mode[15*VVL+il] += fchunk[16*VVL+il]*-c1;
  __targetILP__(il) mode[15*VVL+il] += fchunk[17*VVL+il]*c1;
  __targetILP__(il) mode[15*VVL+il] += fchunk[18*VVL+il]*c0;

  /* m=16*/
  __targetILP__(il) mode[16*VVL+il] += fchunk[0*VVL+il]*c0;
  __targetILP__(il) mode[16*VVL+il] += fchunk[1*VVL+il]*c0;
  __targetILP__(il) mode[16*VVL+il] += fchunk[2*VVL+il]*c0;
  __targetILP__(il) mode[16*VVL+il] += fchunk[3*VVL+il]*c0;
  __targetILP__(il) mode[16*VVL+il] += fchunk[4*VVL+il]*c0;
  __targetILP__(il) mode[16*VVL+il] += fchunk[5*VVL+il]*c0;
  __targetILP__(il) mode[16*VVL+il] += fchunk[6*VVL+il]*c1;
  __targetILP__(il) mode[16*VVL+il] += fchunk[7*VVL+il]*-c1;
  __targetILP__(il) mode[16*VVL+il] += fchunk[8*VVL+il]*c1;
  __targetILP__(il) mode[16*VVL+il] += fchunk[9*VVL+il]*c0;
  __targetILP__(il) mode[16*VVL+il] += fchunk[10*VVL+il]*c0;
  __targetILP__(il) mode[16*VVL+il] += fchunk[11*VVL+il]*-c1;
  __targetILP__(il) mode[16*VVL+il] += fchunk[12*VVL+il]*c1;
  __targetILP__(il) mode[16*VVL+il] += fchunk[13*VVL+il]*-c1;
  __targetILP__(il) mode[16*VVL+il] += fchunk[14*VVL+il]*c0;
  __targetILP__(il) mode[16*VVL+il] += fchunk[15*VVL+il]*c0;
  __targetILP__(il) mode[16*VVL+il] += fchunk[16*VVL+il]*c0;
  __targetILP__(il) mode[16*VVL+il] += fchunk[17*VVL+il]*c0;
  __targetILP__(il) mode[16*VVL+il] += fchunk[18*VVL+il]*c0;

  /* m=17*/
  __targetILP__(il) mode[17*VVL+il] += fchunk[0*VVL+il]*c0;
  __targetILP__(il) mode[17*VVL+il] += fchunk[1*VVL+il]*c0;
  __targetILP__(il) mode[17*VVL+il] += fchunk[2*VVL+il]*-c1;
  __targetILP__(il) mode[17*VVL+il] += fchunk[3*VVL+il]*c0;
  __targetILP__(il) mode[17*VVL+il] += fchunk[4*VVL+il]*c1;
  __targetILP__(il) mode[17*VVL+il] += fchunk[5*VVL+il]*c0;
  __targetILP__(il) mode[17*VVL+il] += fchunk[6*VVL+il]*c1;
  __targetILP__(il) mode[17*VVL+il] += fchunk[7*VVL+il]*c0;
  __targetILP__(il) mode[17*VVL+il] += fchunk[8*VVL+il]*-c1;
  __targetILP__(il) mode[17*VVL+il] += fchunk[9*VVL+il]*c0;
  __targetILP__(il) mode[17*VVL+il] += fchunk[10*VVL+il]*c0;
  __targetILP__(il) mode[17*VVL+il] += fchunk[11*VVL+il]*c1;
  __targetILP__(il) mode[17*VVL+il] += fchunk[12*VVL+il]*c0;
  __targetILP__(il) mode[17*VVL+il] += fchunk[13*VVL+il]*-c1;
  __targetILP__(il) mode[17*VVL+il] += fchunk[14*VVL+il]*c0;
  __targetILP__(il) mode[17*VVL+il] += fchunk[15*VVL+il]*-c1;
  __targetILP__(il) mode[17*VVL+il] += fchunk[16*VVL+il]*c0;
  __targetILP__(il) mode[17*VVL+il] += fchunk[17*VVL+il]*c1;
  __targetILP__(il) mode[17*VVL+il] += fchunk[18*VVL+il]*c0;

  /* m=18*/
  __targetILP__(il) mode[18*VVL+il] += fchunk[0*VVL+il]*c1;
  __targetILP__(il) mode[18*VVL+il] += fchunk[1*VVL+il]*c1;
  __targetILP__(il) mode[18*VVL+il] += fchunk[2*VVL+il]*c1;
  __targetILP__(il) mode[18*VVL+il] += fchunk[3*VVL+il]*-c2;
  __targetILP__(il) mode[18*VVL+il] += fchunk[4*VVL+il]*c1;
  __targetILP__(il) mode[18*VVL+il] += fchunk[5*VVL+il]*c1;
  __targetILP__(il) mode[18*VVL+il] += fchunk[6*VVL+il]*c1;
  __targetILP__(il) mode[18*VVL+il] += fchunk[7*VVL+il]*-c2;
  __targetILP__(il) mode[18*VVL+il] += fchunk[8*VVL+il]*c1;
  __targetILP__(il) mode[18*VVL+il] += fchunk[9*VVL+il]*-c2;
  __targetILP__(il) mode[18*VVL+il] += fchunk[10*VVL+il]*-c2;
  __targetILP__(il) mode[18*VVL+il] += fchunk[11*VVL+il]*c1;
  __targetILP__(il) mode[18*VVL+il] += fchunk[12*VVL+il]*-c2;
  __targetILP__(il) mode[18*VVL+il] += fchunk[13*VVL+il]*c1;
  __targetILP__(il) mode[18*VVL+il] += fchunk[14*VVL+il]*c1;
  __targetILP__(il) mode[18*VVL+il] += fchunk[15*VVL+il]*c1;
  __targetILP__(il) mode[18*VVL+il] += fchunk[16*VVL+il]*-c2;
  __targetILP__(il) mode[18*VVL+il] += fchunk[17*VVL+il]*c1;
  __targetILP__(il) mode[18*VVL+il] += fchunk[18*VVL+il]*c1;
 
}

__target__ void d3q19matmult2chunk(double* mode, double* fchunk, int baseIndex)
{

  double ftmp[VVL];

  int il=0;

  
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w0*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -r2*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -r2*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -r2*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[18*VVL+il];
  __targetILP__(il)   fchunk[0*VVL+il] = ftmp[il];
  
  /* p=1*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) fchunk[1*VVL+il] = ftmp[il];
  
  /* p=2*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += -we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) fchunk[2*VVL+il] =  ftmp[il];
  
  /* p=3*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w1*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += -w1*mode[18*VVL+il];
  __targetILP__(il) fchunk[3*VVL+il] =  ftmp[il];
  
  /* p=4*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += -we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) fchunk[4*VVL+il] = ftmp[il];
  
  /* p=5 */
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) fchunk[5*VVL+il] = ftmp[il];
  
  /* p=6 */
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) fchunk[6*VVL+il] =  ftmp[il];
  
  /* p=7 */
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w1*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += -w1*mode[18*VVL+il];
  __targetILP__(il) fchunk[7*VVL+il] = ftmp[il];
  
  /* p=8 */
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) fchunk[8*VVL+il] =  ftmp[il];
  
  /* p=9*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w1*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += -r6*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += -w1*mode[18*VVL+il];
  __targetILP__(il) fchunk[9*VVL+il] = ftmp[il];
  
  /* p=10*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w1*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += -r6*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += -w1*mode[18*VVL+il];
  __targetILP__(il) fchunk[10*VVL+il] = ftmp[il];
  
  /* p=11 */
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) fchunk[11*VVL+il] =  ftmp[il];
  
  /* p=12*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w1*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += -r6*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += -w1*mode[18*VVL+il];
  __targetILP__(il) fchunk[12*VVL+il] = ftmp[il];
  
  /* p=13 */
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) fchunk[13*VVL+il] =  ftmp[il];
  
  /* p=14 */
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) fchunk[14*VVL+il] =  ftmp[il];
  
  /* p=15*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += -we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += -r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) fchunk[15*VVL+il] =  ftmp[il];
  
  /* p=16*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w1*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += -r6*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += r6*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += -r4*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += -w1*mode[18*VVL+il];
  __targetILP__(il) fchunk[16*VVL+il] =ftmp[il];
  
  /* p=17*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += wd*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += -we*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += r8*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) fchunk[17*VVL+il] = ftmp[il];
  
  /* p=18*/
  __targetILP__(il) ftmp[il]=0.;
  __targetILP__(il) ftmp[il] += w2*mode[0*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[1*VVL+il];
  __targetILP__(il) ftmp[il] += -wa*mode[2*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[3*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[4*VVL+il];
  __targetILP__(il) ftmp[il] += r4*mode[5*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[6*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[7*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[8*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[9*VVL+il];
  __targetILP__(il) ftmp[il] += -wb*mode[10*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[11*VVL+il];
  __targetILP__(il) ftmp[il] += wa*mode[12*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[13*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[14*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[15*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[16*VVL+il];
  __targetILP__(il) ftmp[il] += c0*mode[17*VVL+il];
  __targetILP__(il) ftmp[il] += wc*mode[18*VVL+il];
  __targetILP__(il) fchunk[18*VVL+il] = ftmp[il];


}


 __target__ void updateDistD3Q19(double jdotc[3*VVL],double sphidotq[VVL],double sphi[3][3*VVL],double phi[VVL], double jphi[3*VVL], double* t_f, int baseIndex){

  int iv=0;

__targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*-3.3333333333333331e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 0) ] 
        = tc_wv[0]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4)+ phi[iv];


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  jdotc[iv] += jphi[0*VVL+iv]*1;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][1*VVL+iv]*1.0000000000000000e+00;
  __targetILP__(iv)  jdotc[iv] += jphi[1*VVL+iv]*1;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][0*VVL+iv]*1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*-3.3333333333333331e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 1) ] 
        = tc_wv[1]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  jdotc[iv] += jphi[0*VVL+iv]*1;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][2*VVL+iv]*1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  jdotc[iv] += jphi[2*VVL+iv]*1;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][0*VVL+iv]*1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*6.6666666666666663e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 2) ] 
        = tc_wv[2]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  jdotc[iv] += jphi[0*VVL+iv]*1;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*-3.3333333333333331e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 3) ] 
        = tc_wv[3]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  jdotc[iv] += jphi[0*VVL+iv]*1;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][2*VVL+iv]*-1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  jdotc[iv] += jphi[2*VVL+iv]*-1;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][0*VVL+iv]*-1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*6.6666666666666663e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 4) ] 
        = tc_wv[4]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  jdotc[iv] += jphi[0*VVL+iv]*1;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][1*VVL+iv]*-1.0000000000000000e+00;
  __targetILP__(iv)  jdotc[iv] += jphi[1*VVL+iv]*-1;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][0*VVL+iv]*-1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*-3.3333333333333331e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 5) ] 
        = tc_wv[5]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  jdotc[iv] += jphi[1*VVL+iv]*1;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][2*VVL+iv]*1.0000000000000000e+00;
  __targetILP__(iv)  jdotc[iv] += jphi[2*VVL+iv]*1;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][1*VVL+iv]*1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*6.6666666666666663e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 6) ] 
        = tc_wv[6]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  jdotc[iv] += jphi[1*VVL+iv]*1;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*-3.3333333333333331e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 7) ] 
        = tc_wv[7]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  jdotc[iv] += jphi[1*VVL+iv]*1;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][2*VVL+iv]*-1.0000000000000000e+00;
  __targetILP__(iv)  jdotc[iv] += jphi[2*VVL+iv]*-1;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][1*VVL+iv]*-1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*6.6666666666666663e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 8) ] 
        = tc_wv[8]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  jdotc[iv] += jphi[2*VVL+iv]*1;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*6.6666666666666663e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 9) ] 
        = tc_wv[9]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  jdotc[iv] += jphi[2*VVL+iv]*-1;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*6.6666666666666663e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 10) ] 
        = tc_wv[10]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  jdotc[iv] += jphi[1*VVL+iv]*-1;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][2*VVL+iv]*-1.0000000000000000e+00;
  __targetILP__(iv)  jdotc[iv] += jphi[2*VVL+iv]*1;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][1*VVL+iv]*-1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*6.6666666666666663e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 11) ] 
        = tc_wv[11]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  jdotc[iv] += jphi[1*VVL+iv]*-1;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*-3.3333333333333331e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 12) ] 
        = tc_wv[12]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  jdotc[iv] += jphi[1*VVL+iv]*-1;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][2*VVL+iv]*1.0000000000000000e+00;
  __targetILP__(iv)  jdotc[iv] += jphi[2*VVL+iv]*-1;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][1*VVL+iv]*1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*6.6666666666666663e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 13) ] 
        = tc_wv[13]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  jdotc[iv] += jphi[0*VVL+iv]*-1;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][1*VVL+iv]*-1.0000000000000000e+00;
  __targetILP__(iv)  jdotc[iv] += jphi[1*VVL+iv]*1;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][0*VVL+iv]*-1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*-3.3333333333333331e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 14) ] 
        = tc_wv[14]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  jdotc[iv] += jphi[0*VVL+iv]*-1;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][2*VVL+iv]*-1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  jdotc[iv] += jphi[2*VVL+iv]*1;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][0*VVL+iv]*-1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*6.6666666666666663e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 15) ] 
        = tc_wv[15]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  jdotc[iv] += jphi[0*VVL+iv]*-1;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*-3.3333333333333331e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 16) ] 
        = tc_wv[16]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  jdotc[iv] += jphi[0*VVL+iv]*-1;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][2*VVL+iv]*1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*-3.3333333333333331e-01;
  __targetILP__(iv)  jdotc[iv] += jphi[2*VVL+iv]*-1;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][0*VVL+iv]*1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*6.6666666666666663e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 17) ] 
        = tc_wv[17]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


 __targetILP__(iv) { jdotc[iv]    = 0.0; sphidotq[iv] = 0.0;} 

  __targetILP__(iv)  jdotc[iv] += jphi[0*VVL+iv]*-1;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][0*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[0][1*VVL+iv]*1.0000000000000000e+00;
  __targetILP__(iv)  jdotc[iv] += jphi[1*VVL+iv]*-1;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][0*VVL+iv]*1.0000000000000000e+00;
  __targetILP__(iv)  sphidotq[iv] += sphi[1][1*VVL+iv]*6.6666666666666663e-01;
  __targetILP__(iv)  sphidotq[iv] += sphi[2][2*VVL+iv]*-3.3333333333333331e-01;

 __targetILP__(iv) 
     t_f[ LB_ADDR(tc_nSites, NDIST, NVEL, baseIndex+iv, 1, 18) ] 
        = tc_wv[18]*(jdotc[iv]*tc_rcs2 + sphidotq[iv]*tc_r2rcs4);


  return;

 }
