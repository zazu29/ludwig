/*****************************************************************************
 *
 *  colloids_Q_tensor.c
 *
 *  Routines to set the Q tensor inside a colloid to correspond
 *  to homeotropic (normal) or planar anchoring at the surface.
 *
 *  $Id$
 *
 *  Edinburgh Soft Matter and Statistical Physics Group and
 *  Edinburgh Parallel Computing Centre
 *
 *  Juho Lintuvuori (jlintuvu@ph.ed.ac.uk)
 *  (c) 2011 The University of Edinburgh
 *  
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>

#include "pe.h"
#include "build.h"
#include "coords.h"
#include "colloids.h"
#include "phi.h"
#include "io_harness.h"
#include "ran.h"
#include "lattice.h"
#include "util.h"
#include "model.h"
#include "site_map.h"
#include "colloids_Q_tensor.h"

struct io_info_t * io_info_scalar_q_;
static int anchoring_ = ANCHORING_NORMAL;
static int anchoring_method_ = ANCHORING_METHOD_NONE;
static double w_surface_ = 0.0; /* Anchoring strength in free energy */

static int scalar_q_dir_write(FILE * fp, const int i, const int j, const int k);
static int scalar_q_dir_write_ascii(FILE *, const int, const int, const int);
static void scalar_order_parameter_director(double q[3][3], double qs[4]);

void COLL_set_Q(){

  int ia;
  int ic,jc,kc;
  
  colloid_t * p_colloid;

  double rsite0[3];
  double normal[3];
  double dir[3];

  colloid_t * colloid_at_site_index(int);

  int nlocal[3],offset[3];
  int index;

  double q[3][3];
  double qs[4];
  double director[3];
  double len_normal;
  double rlen_normal;
  double amplitude;
  double rdotd;
  double dir_len;

  amplitude = 0.33333333;

  coords_nlocal(nlocal);
  coords_nlocal_offset(offset);
  
  for (ic = 1; ic <= nlocal[X]; ic++) {
    for (jc = 1; jc <= nlocal[Y]; jc++) {
      for (kc = 1; kc <= nlocal[Z]; kc++) {
	
	index = coords_index(ic, jc, kc);

	p_colloid = colloid_at_site_index(index);
	
	/* check if this node is inside a colloid */

	if (p_colloid != NULL){	  

	  /* Need to translate the colloid position to "local"
	   * coordinates, so that the correct range of lattice
	   * nodes is found */

	  rsite0[X] = 1.0*(offset[X] + ic);
	  rsite0[Y] = 1.0*(offset[Y] + jc);
	  rsite0[Z] = 1.0*(offset[Z] + kc);

	  /* calculate the vector between the centre of mass of the
	   * colloid and node i, j, k 
	   * so need to calculate rsite0 - r0 */

	  normal[X] = rsite0[X] - p_colloid->s.r[X];
	  normal[Y] = rsite0[Y] - p_colloid->s.r[Y];
	  normal[Z] = rsite0[Z] - p_colloid->s.r[Z];

	  /* now for homeotropic anchoring only thing needed is to
	   * normalise the the surface normal vector  */

	  len_normal = modulus(normal);
	  assert(len_normal <= p_colloid->s.ah);

	  if (len_normal < 10e-8) {
	    /* we are very close to the centre of the colloid.
	     * set the q tensor to zero */
	    q[X][X] = 0.0;
	    q[X][Y] = 0.0;
	    q[X][Z] = 0.0;
	    q[Y][Y] = 0.0;
	    q[Y][Z] = 0.0;
	    phi_set_q_tensor(index,q);
	    continue;
	  }

	  rlen_normal = 1.0/len_normal;

	  /* Homeotropic anchoring (the default) */

	  director[X] = normal[X]*rlen_normal;
	  director[Y] = normal[Y]*rlen_normal;
	  director[Z] = normal[Z]*rlen_normal;
	  
	  if (anchoring_ == ANCHORING_PLANAR) {

	    /* now we need set the director inside the colloid
	     * perpendicular to the vector normal of of the surface
	     * [i.e. it is confined in a plane] i.e.  
	     * perpendicular to the vector r between the centre of the colloid
	     * corresponding node i,j,k
	     * -Juho
	     */
	  
	    phi_get_q_tensor(index, q);
	    scalar_order_parameter_director(q, qs);

	    dir[X] = qs[1];
	    dir[Y] = qs[2];
	    dir[Z] = qs[3];

	    /* calculate the projection of the director along the surface
	     * normal and remove that from the director to make the
	     * director perpendicular to the surface */

	    rdotd = dot_product(normal, dir)*rlen_normal*rlen_normal;
	    
	    dir[X] = dir[X] - rdotd*normal[X];
	    dir[Y] = dir[Y] - rdotd*normal[Y];
	    dir[Z] = dir[Z] - rdotd*normal[Z];
	    
	    dir_len = modulus(dir);

	    if (dir_len < 10e-8) {
	      /* the vectors were [almost] parallel.
	       * now we use the direction of the previous node i,j,k
	       * this fails if we are in the first node, so not great fix...
	       */

	      fatal("dir_len < 10-8 i,j,k, %d %d %d\n", ic,jc,kc);
	    }

	    for (ia = 0; ia < 3; ia++) {
	      director[ia] = dir[ia] / dir_len;
	    }
	  }

	  q[X][X] = 1.5*amplitude*(director[X]*director[X] - 1.0/3.0);
	  q[X][Y] = 1.5*amplitude*(director[X]*director[Y]);
	  q[X][Z] = 1.5*amplitude*(director[X]*director[Z]);
	  q[Y][Y] = 1.5*amplitude*(director[Y]*director[Y] - 1.0/3.0);
	  q[Y][Z] = 1.5*amplitude*(director[Y]*director[Z]);
	    
	  phi_set_q_tensor(index, q);

	}
	
      }
    }
  }
  return;
}

/*****************************************************************************
 *
 *  colloids_q_boundary_normal
 *
 *  Find the 'true' outward unit normal at fluid site index. Note that
 *  the half-way point is not used to provide a simple unique value in
 *  the gradient calculation.
 *
 *  The unit lattice vector which is the discrete outward normal is di[3].
 *  The result is returned in unit vector dn.
 *
 *****************************************************************************/

void colloids_q_boundary_normal(const int index, const int di[3],
				double dn[3]) {
  int ia, index1;
  int noffset[3];
  int isite[3];

  double rd;
  colloid_t * pc;
  colloid_t * colloid_at_site_index(int);

  coords_nlocal_offset(noffset);
  coords_index_to_ijk(index, isite);

  index1 = coords_index(isite[X] - di[X], isite[Y] - di[Y], isite[Z] - di[Z]);
  pc = colloid_at_site_index(index1);
  assert(pc);

  for (ia = 0; ia < 3; ia++) {
    dn[ia] = 1.0*(noffset[ia] + isite[ia]);
    dn[ia] -= pc->s.r[ia];
  }

  rd = modulus(dn);
  assert(rd > 0.0);
  rd = 1.0/rd;

  for (ia = 0; ia < 3; ia++) {
    dn[ia] *= rd;
  }

  return;
}

/*****************************************************************************
 *
 *  colloids_q_boundary
 *
 *  Produce an estimate of the surface order parameter Q^0_ab for
 *  normal or planar anchoring.
 *
 *  This will depend on the outward surface normal nhat, and in the
 *  case of planar anchoring may depend on the estimate of the
 *  existing order parameter at the surface Qs_ab.
 *
 *  This planar anchoring idea follows e.g., Fournier and Galatola
 *  Europhys. Lett. 72, 403 (2005).
 *
 *  Fixed planar anchoring can be hardwired at the moment by setting
 *  n_a. This is appropriate for flat walls only.
 *
 *****************************************************************************/

void colloids_q_boundary(const double nhat[3], double qs[3][3],
			 double q0[3][3]) {
  int ia, ib, ic, id;

  double qtilde[3][3];
  double amplitude;
  double  n[3];

  amplitude = 0.33333333;

  if (anchoring_ == ANCHORING_NORMAL) {
    for (ia = 0; ia < 3; ia++) {
      for (ib = 0; ib < 3; ib++) {
	q0[ia][ib] = 0.5*amplitude*(3.0*nhat[ia]*nhat[ib] - d_[ia][ib]);
      }
    }
  }

  if (anchoring_ == ANCHORING_PLANAR) {

    /* Planar: use the fluid Q_ab to find ~Q_ab */

    for (ia = 0; ia < 3; ia++) {
      for (ib = 0; ib < 3; ib++) {
	qtilde[ia][ib] = qs[ia][ib] + 0.5*amplitude*d_[ia][ib];
      }
    }

    for (ia = 0; ia < 3; ia++) {
      for (ib = 0; ib < 3; ib++) {
	q0[ia][ib] = 0.0;
	for (ic = 0; ic < 3; ic++) {
	  for (id = 0; id < 3; id++) {
	    q0[ia][ib] += (d_[ia][ic] - nhat[ia]*nhat[ic])*qtilde[ic][id]
	      *(d_[id][ib] - nhat[id]*nhat[ib]);
	  }
	}
	/* Return Q^0_ab = ~Q_ab - (1/2) A d_ab */
	q0[ia][ib] -= 0.5*amplitude*d_[ia][ib];
      }
    }

  }

  if (anchoring_ == ANCHORING_FIXED) {

    n[X] = 0.0;
    n[Y] = 1.0;
    n[Z] = 0.0;

    for (ia = 0; ia < 3; ia++) {
      for (ib = 0; ib < 3; ib++) {
	q0[ia][ib] = 0.5*amplitude*(3.0*n[ia]*n[ib] - d_[ia][ib]);
      }
    }
  }

  return;
}

void COLL_set_Q_2(){

  int ia;
  int ic,jc,kc;
  
  colloid_t * p_colloid;

  double r0[3];
  double rsite0[3];
  double normal[3];
  double dir[3];
  double dir_prev[3];

  colloid_link_t * p_link;

  int nlocal[3],offset[3];
  int jsite[3];
  double q[3][3];
  double qs[4];
  double director[3];
  double len_normal;
  double rlen_normal;
  double amplitude;
  double rdotd;
  double dir_len;
  amplitude = 0.33333333;

  coords_nlocal(nlocal);
  coords_nlocal_offset(offset);
 
  /* Loop through all cells (including the halo cells) */
  
  for (ic = 0; ic <= Ncell(X) + 1; ic++){
    for (jc = 0; jc <= Ncell(Y) + 1; jc++){
      for (kc = 0; kc <= Ncell(Z) + 1; kc++) {

	/* Set the cell index */

	p_colloid = colloids_cell_list(ic, jc, kc);
	
	/* check if this node is inside a colloid */
	while (p_colloid != NULL){
	  
	  p_link = p_colloid->lnk;

	  /* loop over the links to get access to the boundary nodes */
	  while(p_link != NULL){

	    if (p_link->status == LINK_UNUSED) {
	      /* ignore */
	    }
	    else {
	      
	      /* get the i j k of the boundary node in local coordinates */
	      coords_index_to_ijk(p_link->j, jsite);
  
	      /* Need to translate the colloid position to "local"
	       * coordinates, so that the correct range of lattice
	       * nodes is found */

	      r0[X] = p_colloid->s.r[X] - 1.0*offset[X];
	      r0[Y] = p_colloid->s.r[Y] - 1.0*offset[Y];
	      r0[Z] = p_colloid->s.r[Z] - 1.0*offset[Z];

	      rsite0[X] = 1.0*jsite[0];
	      rsite0[Y] = 1.0*jsite[1];
	      rsite0[Z] = 1.0*jsite[2];

	      /* calculate the vector between the centre of mass of the
	       * colloid and node i, j, k 
	       * so need to calculate rsite0 - r0 */

	      coords_minimum_distance(r0, rsite0, normal);

	      /* now for homeotropic anchoring only thing needed is to
	       * normalise the the surface normal vector  */

	      len_normal = modulus(normal);
	      assert(len_normal <= p_colloid->s.ah);
	    
	      if (len_normal < 10e-8) {
		/* we are very close to the centre of the colloid.
		 * set the q tensor to zero */
		q[X][X] = 0.0;
		q[X][Y] = 0.0;
		q[X][Z] = 0.0;
		q[Y][Y] = 0.0;
		q[Y][Z] = 0.0;
		phi_set_q_tensor(p_link->j,q);
		fatal("Colloid too small, distance %lf\n", len_normal);
		continue;
	      }
	    
	      rlen_normal = 1.0/len_normal;

	      /* Homeotropic anchoring */

	      director[X] = normal[X]*rlen_normal;
	      director[Y] = normal[Y]*rlen_normal;
	      director[Z] = normal[Z]*rlen_normal;

	      if (anchoring_ == ANCHORING_PLANAR) {
	  
		phi_get_q_tensor(p_link->j, q);
		scalar_order_parameter_director(q, qs);
	      
		dir[X] = qs[1];
		dir[Y] = qs[2];
		dir[Z] = qs[3];
	    
		/* calculate the projection of the director along the surface
		 * normal and remove that from the director to make the
		 * director perpendicular to the surface */
	    
		rdotd = dot_product(normal, dir)*rlen_normal*rlen_normal;
	    
		dir[X] = dir[X] - rdotd*normal[X];
		dir[Y] = dir[Y] - rdotd*normal[Y];
		dir[Z] = dir[Z] - rdotd*normal[Z];
	      
		dir_len = modulus(dir);

		if (dir_len < 10e-8) {
		  /* the vectors were [almost] parallel.
		   * now we use the direction of the previous node i,j,k
		   * this fails if we are in the first node.
		   */

		  cross_product(dir_prev, normal, dir);
		  dir_len = modulus(dir);
		  fatal("dir_len < 10-8 i,j,k, %d %d %d\n", ic,jc,kc);
		}

		for (ia = 0; ia < 3; ia++) {
		  director[ia] = dir[ia] / dir_len;
		  dir_prev[ia] = dir[ia] / dir_len;
		}
	      }

	      q[X][X] = 1.5*amplitude*(director[X]*director[X] - 1.0/3.0);
	      q[X][Y] = 1.5*amplitude*(director[X]*director[Y]);
	      q[X][Z] = 1.5*amplitude*(director[X]*director[Z]);
	      q[Y][Y] = 1.5*amplitude*(director[Y]*director[Y] - 1.0/3.0);
	      q[Y][Z] = 1.5*amplitude*(director[Y]*director[Z]);
	    
	      phi_set_q_tensor(p_link->j, q);

	    }
	    /* Next link */
	    p_link = p_link->next;
	  }
	  /* Next colloid */
	  p_colloid = p_colloid->next;
	}
	/* Next cell */
      }
    }
  }
  return;
}

void COLL_randomize_Q(double delta_r){
  
  int ic,jc,kc;
  
  colloid_t * p_colloid;
  colloid_t * colloid_at_site_index(int);

  int nlocal[3],offset[3];
  int index;
  
  double Pi;
  double q[3][3];
  double amplitude,phase1,phase2;
  
  /* set amplitude to something small */
  amplitude = 0.0000001;
  
  coords_nlocal(nlocal);
  coords_nlocal_offset(offset);
  
  Pi = 4.0*atan(1.0);

  for (ic = 1; ic <= nlocal[X]; ic++) {
    for (jc = 1; jc <= nlocal[Y]; jc++) {
      for (kc = 1; kc <= nlocal[Z]; kc++) {
	
	index = coords_index(ic, jc, kc);

	p_colloid = colloid_at_site_index(index);
	
	/* check if this node is inside a colloid */
	if(p_colloid != NULL){
	  phase1= 2.0/5.0*Pi*(0.5-ran_parallel_uniform());
	  phase2= Pi/2.0+Pi/5.0*(0.5-ran_parallel_uniform());

	  q[X][X] = amplitude* (3.0/2.0*sin(phase2)*sin(phase2)*cos(phase1)*cos(phase1)-1.0/2.0);
	  q[X][Y] = 3.0*amplitude/2.0*(sin(phase2)*sin(phase2)*cos(phase1)*sin(phase1));
	  q[X][Z] = 3.0*amplitude/2.0*(sin(phase2)*cos(phase2)*cos(phase1));
	  q[Y][X] = q[X][Y];
	  q[Y][Y] = amplitude*(3.0/2.0*sin(phase2)*sin(phase2)*sin(phase1)*sin(phase1)-1.0/2.0);
	  q[Y][Z] = 3.0*amplitude/2.0*(sin(phase2)*cos(phase2)*sin(phase1));
	  q[Z][X] = q[X][Z];
	  q[Z][Y] = q[Y][Z];
	  q[Z][Z] = - q[X][X] - q[Y][Y];

	  phi_set_q_tensor(index, q);
	}
      }
    }
  }
}

#define ROTATE(a,i,j,k,l) g=a[i][j];h=a[k][l];a[i][j]=g-s*(h+g*tau);\
	a[k][l]=h+s*(g-h*tau);
#define n 3
void jacobi(double (*a)[n], double d[], double (*v)[n], int *nrot)
{
  int j,iq,ip,i;
  double tresh,theta,tau,t,sm,s,h,g,c;
  double b[n],z[n];

  for (ip=0;ip<n;ip++) {
    for (iq=0;iq<n;iq++) v[ip][iq]=0.0;
    v[ip][ip]=1.0;
  }
  for (ip=0;ip<n;ip++) {
    b[ip]=d[ip]=a[ip][ip];
    z[ip]=0.0;
  }
  *nrot=0;
  for (i=1;i<=50;i++) {
    sm=0.0;
    for (ip=0;ip< n-1;ip++) {
      for (iq=ip+1;iq<n;iq++)
	sm += fabs(a[ip][iq]);
    }
    if (sm == 0.0) {
      return;
    }
    if (i < 4)
      tresh=0.2*sm/(n*n);
    else
      tresh=0.0;
    for (ip=0;ip<n-1;ip++) {
      for (iq=ip+1;iq<n;iq++) {
	g=100.0*fabs(a[ip][iq]);
	if (i > 4 && (fabs(d[ip])+g) == fabs(d[ip])
	    && (fabs(d[iq])+g) == fabs(d[iq]))
	  a[ip][iq]=0.0;
	else if (fabs(a[ip][iq]) > tresh) {
	  h=d[iq]-d[ip];
	  if ((fabs(h)+g) == fabs(h))
	    t=(a[ip][iq])/h;
	  else {
	    theta=0.5*h/(a[ip][iq]);
	    t=1.0/(fabs(theta)+sqrt(1.0+theta*theta));
	    if (theta < 0.0) t = -t;
	  }
	  c=1.0/sqrt(1+t*t);
	  s=t*c;
	  tau=s/(1.0+c);
	  h=t*a[ip][iq];
	  z[ip] -= h;
	  z[iq] += h;
	  d[ip] -= h;
	  d[iq] += h;
	  a[ip][iq]=0.0;
	  for (j=0;j<=ip-1;j++) {
	    ROTATE(a,j,ip,j,iq)
	      }
	  for (j=ip+1;j<=iq-1;j++) {
	    ROTATE(a,ip,j,j,iq)
	      }
	  for (j=iq+1;j<n;j++) {
	    ROTATE(a,ip,j,iq,j)
	      }
	  for (j=0;j<n;j++) {
	    ROTATE(v,j,ip,j,iq)
	      }
	  ++(*nrot);
	}
      }
    }
    for (ip=0;ip<n;ip++) {
      b[ip] += z[ip];
      d[ip]=b[ip];
      z[ip]=0.0;
    }
  }
  printf("Too many iterations in routine jacobi");
  exit(0);
}
#undef n
#undef ROTATE

/*****************************************************************************
 *
 *  colloids_fix_swd
 *
 *  The velocity gradient tensor used in the Beris-Edwards equations
 *  requires some approximation to the velocity at lattice sites
 *  inside particles. Here we set the lattice velocity based on
 *  the solid body rotation u = v + Omega x rb
 *
 *****************************************************************************/

void colloids_fix_swd(void) {

  int ic, jc, kc, index;
  int nlocal[3];
  int noffset[3];
  const int nextra = 1;

  double u[3];
  double rb[3];
  double x, y, z;

  colloid_t * p_c;
  colloid_t * colloid_at_site_index(int);

  coords_nlocal(nlocal);
  coords_nlocal_offset(noffset);

  for (ic = 1 - nextra; ic <= nlocal[X] + nextra; ic++) {
    x = noffset[X] + ic;
    for (jc = 1 - nextra; jc <= nlocal[Y] + nextra; jc++) {
      y = noffset[Y] + jc;
      for (kc = 1 - nextra; kc <= nlocal[Z] + nextra; kc++) {
	z = noffset[Z] + kc;

	index = coords_index(ic, jc, kc);

	p_c = colloid_at_site_index(index);

	if (p_c) {
	  /* Set the lattice velocity here to the solid body
	   * rotational velocity */

	  rb[X] = x - p_c->s.r[X];
	  rb[Y] = y - p_c->s.r[Y];
	  rb[Z] = z - p_c->s.r[Z];

	  cross_product(p_c->s.w, rb, u);

	  u[X] += p_c->s.v[X];
	  u[Y] += p_c->s.v[Y];
	  u[Z] += p_c->s.v[Z];

	  hydrodynamics_set_velocity(index, u);

	}
      }
    }
  }

  return;
}

/*****************************************************************************
 *
 *  scalar_q_io_init
 *
 *  Initialise the I/O information for the scalar order parameter and director
 *  output related to blue phase tensor order parameter.
 *
 *  This stuff lives here until a better home is found.
 *
 *****************************************************************************/

void scalar_q_io_info_set(struct io_info_t * info) {

  assert(info);
  io_info_scalar_q_ = info;

  io_info_set_name(io_info_scalar_q_, "Scalar order parameter and director");
  io_info_set_write_binary(io_info_scalar_q_, scalar_q_dir_write);
  io_info_set_write_ascii(io_info_scalar_q_, scalar_q_dir_write_ascii);
  io_info_set_bytesize(io_info_scalar_q_, 4*sizeof(double));

  io_info_set_format_binary(io_info_scalar_q_);
  io_write_metadata("qs_dir", io_info_scalar_q_);

  return;
}

/*****************************************************************************
 *
 *  scalar_q_dir_write_ascii
 *
 *  Write the value of the scalar order parameter and director at (ic, jc, kc).
 *
 *****************************************************************************/

static int scalar_q_dir_write_ascii(FILE * fp, const int ic, const int jc,
				    const int kc) {
  int index, n;
  double q[3][3];
  double qs_dir[4];

  index = coords_index(ic, jc, kc);

  if (site_map_get_status_index(index) == FLUID) {
    phi_get_q_tensor(index, q);
    scalar_order_parameter_director(q, qs_dir);
  }
  else {
    qs_dir[0] = 0.0;
    qs_dir[1] = 0.0;
    qs_dir[2] = 0.0;
    qs_dir[3] = 0.0;
  }

  n = fprintf(fp, "%le %le %le %le\n", qs_dir[0], qs_dir[1], qs_dir[2], qs_dir[3]);
  if (n < 0) fatal("fprintf(qs_dir) failed at index %d\n", index);

  return n;
}


/*****************************************************************************
 *
 *  scalar_q_dir_write
 *
 *  Write scalar order parameter and director in binary.
 *
 *****************************************************************************/

static int scalar_q_dir_write(FILE * fp, const int ic, const int jc,
			      const int kc) {
  int index, n;
  double q[3][3];
  double qs_dir[4];

  index = coords_index(ic, jc, kc);
  phi_get_q_tensor(index, q);
  scalar_order_parameter_director(q, qs_dir);

  if (site_map_get_status_index(index) != FLUID) {
    qs_dir[0] = 1.0;
  }

  n = fwrite(qs_dir, sizeof(double), 4, fp);
  if (n != 4) fatal("fwrite(qs_dir) failed at index %d\n", index);

  return n;
}

/*****************************************************************************
 *
 *  scalar_order_parameter_director
 *
 *  Return the value of the scalar order parameter and director for
 *  given Q tensor.
 *
 *****************************************************************************/

static void scalar_order_parameter_director(double q[3][3], double qs[4]) {

  int nrots, emax;
  double d[3], v[3][3];

  jacobi(q, d, v, &nrots);
  
  /* Find the largest eigenvalue and director */

  if (d[0] > d[1]) {
    emax=0;
  }
  else {
    emax=1;
  }
  if (d[2] > d[emax]) {
    emax=2;
  }

  qs[0] = d[emax];
  qs[1] = v[X][emax];
  qs[2] = v[Y][emax];
  qs[3] = v[Z][emax];

  return;
}

/*****************************************************************************
 *
 *  colloids_q_tensor_anchoring_set
 *
 *****************************************************************************/

void colloids_q_tensor_anchoring_set(const int type) {

  assert(type == ANCHORING_PLANAR || type == ANCHORING_NORMAL ||
	 type == ANCHORING_FIXED);

  anchoring_ = type;

  return;
}

/*****************************************************************************
 *
 *  colloids_q_tensor_w
 *
 *****************************************************************************/

double colloids_q_tensor_w(void) {

  return w_surface_;
}

/*****************************************************************************
 *
 *  colloids_q_tensor_w_set
 *
 *****************************************************************************/

void colloids_q_tensor_w_set(double w) {

  w_surface_ = w;
  return;
}

/*****************************************************************************
 *
 *  colloids_q_anchoring_method_set
 *
 *****************************************************************************/

void colloids_q_anchoring_method_set(int method) {

  assert(method == ANCHORING_METHOD_NONE ||
	 method == ANCHORING_METHOD_ONE ||
	 method == ANCHORING_METHOD_TWO);

  anchoring_method_ = method;

  return;
}

/*****************************************************************************
 *
 *  colloids_q_anchoring_method
 *
 *****************************************************************************/

int colloids_q_anchoring_method(void) {

  return anchoring_method_;
}
