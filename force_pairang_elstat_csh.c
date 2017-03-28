/****************************************************************
 *
 * force_pairang_elstat_csh.c: Routine used for calculating pair/monopole
 * and angular interactions forces/energies with coreshell switch
 *
 ****************************************************************
 *
 * Copyright 2002-2014
 *	Institute for Theoretical and Applied Physics
 *	University of Stuttgart, D-70550 Stuttgart, Germany
 *	http://potfit.sourceforge.net/
 *
 *      PAIRANG elstat potential: Ariel Lozano
 *
 ****************************************************************
 *
 *   This file is part of potfit.
 *
 *   potfit is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   potfit is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with potfit; if not, see <http://www.gnu.org/licenses/>.
 *
 ****************************************************************/

#include "potfit.h"

#if defined ANG && defined COULOMB && defined CSH

#include "functions.h"
#include "potential.h"
#include "splines.h"
#include "utils.h"

/****************************************************************
 *
 *  compute forces using eam potentials with spline interpolation
 *
 *  returns sum of squares of differences between calculated and reference
 *     values
 *
 *  arguments: *xi - pointer to potential
 *             *forces - pointer to forces calculated from potential
 *             flag - used for special tasks
 *
 * When using the mpi-parallelized version of potfit, all processes but the
 * root process jump into this function immediately after initialization and
 * stay in here for an infinite loop, to exit only when a certain flag value
 * is passed from process 0. When a set of forces needs to be calculated,
 * the root process enters the function with a flag value of 0, broadcasts
 * the current potential table xi and the flag value to the other processes,
 * thus initiating a force calculation. Whereas the root process returns with
 * the result, the other processes stay in the loop. If the root process is
 * called with flag value 1, all processes exit the function without
 * calculating the forces.
 * If anything changes about the potential beyond the values of the parameters,
 * e.g. the location of the sampling points, these changes have to be broadcast
 * from rank 0 process to the higher ranked processes. This is done when the
 * root process is called with flag value 2. Then a potsync function call is
 * initiated by all processes to get the new potential from root.
 *
 * xi_opt is the array storing the potential parameters (usually it is the
 *     opt_pot.table - part of the struct opt_pot, but it can also be
 *     modified from the current potential.
 *
 * forces is the array storing the deviations from the reference data, not
 *     only for forces, but also for energies, stresses or dummy constraints
 *     (if applicable).
 *
 * flag is an integer controlling the behaviour of calc_forces_eam.
 *    flag == 1 will cause all processes to exit calc_forces_eam after
 *             calculation of forces.
 *    flag == 2 will cause all processes to perform a potsync (i.e. broadcast
 *             any changed potential parameters from process 0 to the others)
 *             before calculation of forces
 *    all other values will cause a set of forces to be calculated. The root
 *             process will return with the sum of squares of the forces,
 *             while all other processes remain in the function, waiting for
 *             the next communication initiating another force calculation
 *             loop
 *
 ****************************************************************/

double calc_forces(double *xi_opt, double *forces, int flag)
{
  int   first, col, i = flag;
  double *xi = NULL;

  /* Some useful temp variables */
  double tmpsum = 0.0, sum = 0.0;
  double angener_sum = 0.0;
  int  ne, size;
  apot_table_t *apt = &apot_table;
  double charge[ntypes];
  double sum_charges;
  double dp_kappa;
  int  self;
  int  type1, type2;
  double fnval, grad, fnval_tail, grad_tail, grad_i, grad_j;

  /* Temp variables */
  atom_t *atom;			/* atom pointer */
  int   h, j, k;
  int   n_i, n_j, n_k;
  int   uf;
#ifdef APOT
  double temp_eng;
#endif /* APOT */
#ifdef STRESS
  int   us, stresses;
#endif /* STRESS */

  /* Some useful temp struct variable types */
  /* neighbor pointers */
  neigh_t *neigh_j, *neigh_k;

  /* Pair variables */
  double phi_val, phi_grad;
  vector tmp_force;

  /* Angular derivatives variables */
  double dV3j, dV3k, V3, vlj, vlk, vv3j, vv3k;
  vector dfj, dfk;
  angle_t *angle;

  switch (format) {
      case 0:
	xi = calc_pot.table;
	break;
      case 3:			/* fall through */
      case 4:
	xi = xi_opt;		/* calc-table is opt-table */
	break;
      case 5:
	xi = calc_pot.table;	/* we need to update the calc-table */
  }

  ne = apot_table.total_ne_par;
  size = apt->number;

  /* This is the start of an infinite loop */
  while (1) {

    /* Reset tmpsum 
       tmpsum = Sum of all the forces, energies and constraints */
    tmpsum = 0.0;

#if defined APOT && !defined MPI
    if (0 == format) {
      apot_check_params(xi_opt);
      update_calc_table(xi_opt, xi, 0);
    }
#endif /* APOT && !MPI */

#ifdef MPI
    /* exchange potential and flag value */
#ifndef APOT
    MPI_Bcast(xi, calc_pot.len, MPI_DOUBLE, 0, MPI_COMM_WORLD);
#endif /* APOT */
    MPI_Bcast(&flag, 1, MPI_INT, 0, MPI_COMM_WORLD);

    if (1 == flag)
      break;			/* Exception: flag 1 means clean up */

#ifdef APOT
    if (0 == myid)
      apot_check_params(xi_opt);
    MPI_Bcast(xi_opt, ndimtot, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    update_calc_table(xi_opt, xi, 0);
#else
    /* if flag==2 then the potential parameters have changed -> sync */
    if (2 == flag)
      potsync();
#endif /* APOT */
#endif /* MPI */

    /* local arrays for electrostatic parameters */
    sum_charges = 0;
    for (i = 0; i < ntypes - 1; i++) {
      if (xi_opt[2 * size + ne + i]) {
	charge[i] = xi_opt[2 * size + ne + i];
	sum_charges += apt->ratio[i] * charge[i];
      } else {
	charge[i] = 0.0;
      }
    }
    apt->last_charge = -sum_charges / apt->ratio[ntypes - 1];
    charge[ntypes - 1] = apt->last_charge;
    if (xi_opt[2 * size + ne + ntypes - 1]) {
      dp_kappa = xi_opt[2 * size + ne + ntypes - 1];
    } else {
      dp_kappa = 0.0;
    }

    /* First step is to initialize 2nd derivatives for splines */

    /* Pair potential (phi)
       where paircol is number of pair potential columns */
    for (col = 0; col < 2 * paircol + ntypes; col++) {
      /* Pointer to first entry */
      first = calc_pot.first[col];

      /* Initialize 2nd derivatives
         step = width of spline knots (known as h)
         xi+first = array with spline values
         calc_pot.last[col1] - first + 1 = num of spline pts
         *(xi + first - 2) = value of endpoint gradient (default: 1e30)
         *(xi + first - 1) = value of other endpoint gradient
         (default: phi=0.0)
         calc_pot.d2tab + first = array to hold 2nd deriv */
      spline_ed(calc_pot.step[col], xi + first, calc_pot.last[col] - first + 1,
	*(xi + first - 2), *(xi + first - 1), calc_pot.d2tab + first);
    }

#ifndef MPI
    myconf = nconf;
#endif /* MPI */

    /* region containing loop over configurations */
    {

      /* Loop over configurations */
      for (h = firstconf; h < firstconf + myconf; h++) {
	uf = conf_uf[h - firstconf];
#ifdef STRESS
	us = conf_us[h - firstconf];
#endif /* STRESS */
	/* Reset energies */
	forces[energy_p + h] = 0.0;
#ifdef STRESS
	/* Reset stresses */
	stresses = stress_p + 6 * h;
	for (i = 0; i < 6; ++i)
	  forces[stresses + i] = 0.0;
#endif /* STRESS */

	/* FIRST LOOP: Reset forces and densities for each atom */
	for (i = 0; i < inconf[h]; i++) {
	  /* Skip every 3 spots in force array starting from position of first atom */
	  n_i = 3 * (cnfstart[h] + i);
	  if (uf) {
	    /* Set initial forces to negative of user given forces so we can take difference */
	    forces[n_i + 0] = -force_0[n_i + 0];
	    forces[n_i + 1] = -force_0[n_i + 1];
	    forces[n_i + 2] = -force_0[n_i + 2];
	  } else {
	    /* Set initial forces to zero if not using forces */
	    forces[n_i + 0] = 0.0;
	    forces[n_i + 1] = 0.0;
	    forces[n_i + 2] = 0.0;
	  }			/* uf */
	}			/* i */
	/* END OF FIRST LOOP */

	/* SECOND LOOP: Calculate pair and monopole forces and energies */
	for (i = 0; i < inconf[h]; i++) {
	  /* Set pointer to temp atom pointer */
	  atom = conf_atoms + (cnfstart[h] - firstatom + i);
	  type1 = atom->type;
	  /* Skip every 3 spots for force array */
	  n_i = 3 * (cnfstart[h] + i);
	  /* Loop over neighbors */
	  for (j = 0; j < atom->num_neigh; j++) {
	    /* Set pointer to temp neighbor pointer */
	    neigh_j = atom->neigh + j;
	    type2 = neigh_j->type;
	    /* Find the correct column in the potential table for pair potential: phi_ij
	       For Binary Alloy: 0 = phi_AA, 1 = (phi_AB or phi_BA), 2 = phi_BB
	       where typ = A = 0 and typ = B = 1 */
	    /* We need to check that neighbor atom exists inside pair potential's radius */
	    if (neigh_j->r < calc_pot.end[neigh_j->col[0]]) {
	      /* Compute phi and phi' value given radial distance
	         NOTE: slot = spline point index right below radial distance
	         shift = % distance from 'slot' spline pt
	         step = width of spline points (given as 'h' in books)
	         0 means the pair potential columns */
	      /* fn value and grad are calculated in the same step */
	      if (uf)
		phi_val =
		  splint_comb_dir(&calc_pot, xi, neigh_j->slot[0], neigh_j->shift[0], neigh_j->step[0],
		  &phi_grad);
	      else
		phi_val = splint_dir(&calc_pot, xi, neigh_j->slot[0], neigh_j->shift[0], neigh_j->step[0]);

	      /* Add in piece contributed by neighbor to energy */
	      forces[energy_p + h] += 0.5 * phi_val;

	      if (uf) {
		/* Compute tmp force values */
		tmp_force.x = neigh_j->dist_r.x * phi_grad;
		tmp_force.y = neigh_j->dist_r.y * phi_grad;
		tmp_force.z = neigh_j->dist_r.z * phi_grad;
		/* Add in force on atom i from atom j */
		forces[n_i + 0] += tmp_force.x;
		forces[n_i + 1] += tmp_force.y;
		forces[n_i + 2] += tmp_force.z;
#ifdef STRESS
		if (us) {
		  /* also calculate pair stresses */
		  forces[stresses + 0] -= 0.5 * neigh_j->dist.x * tmp_force.x;
		  forces[stresses + 1] -= 0.5 * neigh_j->dist.y * tmp_force.y;
		  forces[stresses + 2] -= 0.5 * neigh_j->dist.z * tmp_force.z;
		  forces[stresses + 3] -= 0.5 * neigh_j->dist.x * tmp_force.y;
		  forces[stresses + 4] -= 0.5 * neigh_j->dist.y * tmp_force.z;
		  forces[stresses + 5] -= 0.5 * neigh_j->dist.z * tmp_force.x;
		}
#endif /* STRESS */
	      }
	    }

	    /* calculate monopole forces */
	    /* updating tail-functions - only necessary with variing kappa */
	    if (!apt->sw_kappa)
#ifdef DSF
	      elstat_dsf(neigh_j->r, dp_kappa, &neigh_j->fnval_el, &neigh_j->grad_el, &neigh_j->ggrad_el);
#else
	      elstat_shift(neigh_j->r, dp_kappa, &neigh_j->fnval_el, &neigh_j->grad_el, &neigh_j->ggrad_el);
#endif //DSF

	    /* In small cells, an atom might interact with itself */
	    self = (neigh_j->nr == i + cnfstart[h]) ? 1 : 0;

	    if (neigh_j->r < dp_cut && (charge[type1] || charge[type2])) {

	      fnval_tail = neigh_j->fnval_el;
	      grad_tail = neigh_j->grad_el;

	      grad_i = charge[type2] * grad_tail;
	      if (type1 == type2) {
		grad_j = grad_i;
	      } else {
		grad_j = charge[type1] * grad_tail;
	      }
	      fnval = charge[type1] * charge[type2] * fnval_tail;
	      grad = charge[type1] * grad_i;

	      /* check if pair is a core-shell one
	         and suppress coulomb contribution */
              if ( apot_table.cweight[neigh_j->col[0]] == 0 ) {
                if ( neigh_j->r <= calc_pot.end[neigh_j->col[0]] ) {
                  fnval -= dp_eps * charge[type1] * charge[type2] *
			   neigh_j->inv_r;
                  grad=0;
                }
              }

	      if (self) {
		grad_i *= 0.5;
		grad_j *= 0.5;
		fnval *= 0.5;
		grad *= 0.5;
	      }

	      forces[energy_p + h] += 0.5 * fnval;

	      if (uf) {
		tmp_force.x = 0.5 * neigh_j->dist.x * grad;
		tmp_force.y = 0.5 * neigh_j->dist.y * grad;
		tmp_force.z = 0.5 * neigh_j->dist.z * grad;
		forces[n_i + 0] += tmp_force.x;
		forces[n_i + 1] += tmp_force.y;
		forces[n_i + 2] += tmp_force.z;
		/* actio = reactio */
		n_j = 3 * neigh_j->nr;
		forces[n_j + 0] -= tmp_force.x;
		forces[n_j + 1] -= tmp_force.y;
		forces[n_j + 2] -= tmp_force.z;
#ifdef STRESS
		/* calculate coulomb stresses */
		if (us) {
		  forces[stresses + 0] -= neigh_j->dist.x * tmp_force.x;
		  forces[stresses + 1] -= neigh_j->dist.y * tmp_force.y;
		  forces[stresses + 2] -= neigh_j->dist.z * tmp_force.z;
		  forces[stresses + 3] -= neigh_j->dist.x * tmp_force.y;
		  forces[stresses + 4] -= neigh_j->dist.y * tmp_force.z;
		  forces[stresses + 5] -= neigh_j->dist.z * tmp_force.x;
		}
#endif /* STRESS */
	      }
	    }

	    /* Compute the f_ij values and store the fn and grad in each neighbor struct for easy access later */

	    /* Find the correct column in the potential table for "f": f_ij
	       For Binary Alloy: 0 = f_AA, 1 = f_AB, f_BA, 2 = f_BB
	       where typ = A = 0 and typ = B = 1
	       Note: it is "paircol+2*ntypes" spots away in the array */

	    /* Check that atom j lies inside f_col1 */
	    if (neigh_j->r < calc_pot.end[neigh_j->col[1]]) {
	      /* Store the f(r_ij) value and the gradient for future use */
	      neigh_j->f =
		splint_comb_dir(&calc_pot, xi, neigh_j->slot[1], neigh_j->shift[1], neigh_j->step[1],
		&neigh_j->df);
	    } else {
	      /* Store f and f' = 0 if doesn't lie in boundary to be used later when calculating forces */
	      neigh_j->f = 0.0;
	      neigh_j->df = 0.0;
	    }
	    /* END LOOP OVER NEIGHBORS */
	  }

	  /* Find the correct column in the potential table for angle part: g_ijk
	     Binary Alloy: 0 = g_A, 1 = g_B
	     where A, B are atom type for the main atom i
	     Note: it is now "2*paircol" from beginning column
	     to account for phi(paircol)+f(paircol)
	     col2 = 2 * paircol + typ1; */

	  /* Loop over every angle formed by neighbors
	     N(N-1)/2 possible combinations
	     Used in computing angular part g_ijk */

	  /* set angl pointer to angl_part of current atom */
	  angle = atom->angle_part;

          /* Reset sum of angular component for i */
          angener_sum = 0.0;

	  for (j = 0; j < atom->num_neigh - 1; j++) {
            /* Get pointer to neighbor jj */
            neigh_j = atom->neigh + j;
            /* check that j lies inside f_ij */
            if (neigh_j->r < calc_pot.end[neigh_j->col[1]]) {

              for (k = j + 1; k < atom->num_neigh; k++) {
                /* Get pointer to neighbor kk */
                neigh_k = atom->neigh + k;
                /* check that k lies inside f_ik */
                if (neigh_k->r < calc_pot.end[neigh_k->col[1]]) {

                  /* The cos(theta) should always lie inside -1 ... 1
                   So store the g and g' without checking bounds */
                  angle->g = splint_comb_dir(&calc_pot, xi, angle->slot, angle->shift, angle->step, &angle->dg);

                  /* Sum up angular contribution for atom i caused by j and k
                   f_ij * f_ik * m_ijk */
                  angener_sum += neigh_j->f * neigh_k->f * angle->g;

                  /* Increase angl pointer */
                  angle++;
                }
              }
            }
	  }

	  forces[energy_p + h] += angener_sum;

          /* Compute Angular Forces */
	  if (uf) {
	    /********************************/

	    /* Loop over every angle formed by neighbors
	       N(N-1)/2 possible combinations
	       Used in computing angular part g_ijk */

	    /* set angle pointer to angl_part of current atom */
	    angle = atom->angle_part;
          
            for (j = 0; j < atom->num_neigh - 1; j++) {
              /* Get pointer to neighbor jj */
              neigh_j = atom->neigh + j;
              /* check that j lies inside f_ij */
              if (neigh_j->r < calc_pot.end[neigh_j->col[1]]) {

                /* Force location for atom j */
                n_j = 3 * neigh_j->nr;
                for (k = j + 1; k < atom->num_neigh; k++) {
                  /* Get pointer to neighbor kk */
                  neigh_k = atom->neigh + k;
                  /* check that k lies inside f_ik */
                  if (neigh_k->r < calc_pot.end[neigh_k->col[1]]) {

                    /* Force location for atom k */
                    n_k = 3 * neigh_k->nr;

                    /* Some tmp variables to clean up force fn below */
                    dV3j = angle->g * neigh_j->df * neigh_k->f;
                    dV3k = angle->g * neigh_j->f * neigh_k->df;
                    V3 = neigh_j->f * neigh_k->f * angle->dg;

                    vlj = V3 * neigh_j->inv_r;
                    vlk = V3 * neigh_k->inv_r;
                    vv3j = dV3j - vlj * angle->cos;
                    vv3k = dV3k - vlk * angle->cos;

                    dfj.x = vv3j * neigh_j->dist_r.x + vlj * neigh_k->dist_r.x;
                    dfj.y = vv3j * neigh_j->dist_r.y + vlj * neigh_k->dist_r.y;
                    dfj.z = vv3j * neigh_j->dist_r.z + vlj * neigh_k->dist_r.z;

                    dfk.x = vv3k * neigh_k->dist_r.x + vlk * neigh_j->dist_r.x;
                    dfk.y = vv3k * neigh_k->dist_r.y + vlk * neigh_j->dist_r.y;
                    dfk.z = vv3k * neigh_k->dist_r.z + vlk * neigh_j->dist_r.z;

                    /* Force on atom i from j and k */
                    forces[n_i + 0] += (dfj.x + dfk.x);
                    forces[n_i + 1] += (dfj.y + dfk.y);
                    forces[n_i + 2] += (dfj.z + dfk.z);

                    /* Reaction force on atom j from i and k */
                    forces[n_j + 0] -= dfj.x;
                    forces[n_j + 1] -= dfj.y;
                    forces[n_j + 2] -= dfj.z;

                    /* Reaction force on atom k from i and j */
                    forces[n_k + 0] -= dfk.x;
                    forces[n_k + 1] -= dfk.y;
                    forces[n_k + 2] -= dfk.z;

#ifdef STRESS
                    if (us) {
                      /* Force from j on atom i */
                      tmp_force.x = dfj.x;
                      tmp_force.y = dfj.y;
                      tmp_force.z = dfj.z;
                      forces[stresses + 0] -= neigh_j->dist.x * tmp_force.x;
                      forces[stresses + 1] -= neigh_j->dist.y * tmp_force.y;
                      forces[stresses + 2] -= neigh_j->dist.z * tmp_force.z;
                      forces[stresses + 3] -= neigh_j->dist.x * tmp_force.y;
                      forces[stresses + 4] -= neigh_j->dist.y * tmp_force.z;
                      forces[stresses + 5] -= neigh_j->dist.z * tmp_force.x;

                      /* Force from k on atom i */
                      tmp_force.x = dfk.x;
                      tmp_force.y = dfk.y;
                      tmp_force.z = dfk.z;
                      forces[stresses + 0] -= neigh_k->dist.x * tmp_force.x;
                      forces[stresses + 1] -= neigh_k->dist.y * tmp_force.y;
                      forces[stresses + 2] -= neigh_k->dist.z * tmp_force.z;
                      forces[stresses + 3] -= neigh_k->dist.x * tmp_force.y;
                      forces[stresses + 4] -= neigh_k->dist.y * tmp_force.z;
                      forces[stresses + 5] -= neigh_k->dist.z * tmp_force.x;
                    }
#endif // STRESS
                    /* Increase n_angl pointer */
                    angle++;
                  }
                } 		/* End inner loop over angles (neighbor atom k) */
              }
	    } 			/* End outer loop over angles (neighbor atom j) */
          } 			/* uf */
	}			/* END OF SECOND LOOP OVER ATOM i */

	/* 3RD LOOP OVER ATOM i */
	/* Sum up the square of the forces for each atom
	   then multiply it by the weight for this config */
	double qq;
#ifdef DSF
	double fnval_cut, gtail_cut, ggrad_cut;
        elstat_value(dp_cut, dp_kappa, &fnval_cut, &gtail_cut, &ggrad_cut);
#endif //DSF
	for (i = 0; i < inconf[h]; i++) {
	  atom = conf_atoms + i + cnfstart[h] - firstatom;
	  type1 = atom->type;
	  n_i = 3 * (cnfstart[h] + i);

	  /* self energy contributions */
	  if (charge[type1]) {
	    qq = charge[type1] * charge[type1];
#ifdef DSF
            fnval = qq * ( dp_eps * dp_kappa / sqrt(M_PI) +
	       (fnval_cut - gtail_cut * dp_cut * dp_cut )*0.5 );
#else
	    fnval = dp_eps * dp_kappa * qq / sqrt(M_PI);
#endif //DSF
	    forces[energy_p + h] -= fnval;
	  }
#ifdef FWEIGHT
	  /* Weigh by absolute value of force */
	  forces[n_i + 0] /= FORCE_EPS + atom->absforce;
	  forces[n_i + 1] /= FORCE_EPS + atom->absforce;
	  forces[n_i + 2] /= FORCE_EPS + atom->absforce;
#endif /* FWEIGHT */

#ifdef CONTRIB
	  if (atom->contrib)
#endif /* CONTRIB */
	    tmpsum += conf_weight[h] *
	      (dsquare(forces[n_i + 0]) + dsquare(forces[n_i + 1]) + dsquare(forces[n_i + 2]));
	}			/* END OF THIRD LOOP OVER ATOM i */

	/* Add in the energy per atom and its weight to the sum */
	/* First divide by num atoms */
	forces[energy_p + h] /= (double)inconf[h];

	/* Then subtract off the cohesive energy given to use by user */
	forces[energy_p + h] -= force_0[energy_p + h];

	/* Sum up square of this new energy term for each config
	   multiplied by its respective weight */
	tmpsum += conf_weight[h] * eweight * dsquare(forces[energy_p + h]);

#ifdef STRESS
	/* LOOP OVER STRESSES */
	for (i = 0; i < 6; ++i) {
	  /* Multiply weight to stresses and divide by volume */
	  forces[stresses + i] /= conf_vol[h - firstconf];
	  /* Subtract off user supplied stresses */
	  forces[stresses + i] -= force_0[stresses + i];
	  /* Sum in the square of each stress component with config weight */
	  tmpsum += conf_weight[h] * sweight * dsquare(forces[stresses + i]);
	}
#endif /* STRESS */

      }				/* END MAIN LOOP OVER CONFIGURATIONS */
    }

    /* dummy constraints (global) */
#ifdef APOT
    /* add punishment for out of bounds (mostly for powell_lsq) */
    if (0 == myid) {
      tmpsum += apot_punish(xi_opt, forces);
    }
#endif /* APOT */

#ifdef MPI
    /* Reduce the global sum from all the tmpsum's */
    sum = 0.0;
    MPI_Reduce(&tmpsum, &sum, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    /* gather forces, energies, stresses */
    if (myid == 0) {		/* root node already has data in place */
      /* forces */
      MPI_Gatherv(MPI_IN_PLACE, myatoms, MPI_VECTOR, forces,
	atom_len, atom_dist, MPI_VECTOR, 0, MPI_COMM_WORLD);
      /* energies */
      MPI_Gatherv(MPI_IN_PLACE, myconf, MPI_DOUBLE, forces + energy_p,
	conf_len, conf_dist, MPI_DOUBLE, 0, MPI_COMM_WORLD);
#ifdef STRESS
      /* stresses */
      MPI_Gatherv(MPI_IN_PLACE, myconf, MPI_STENS, forces + stress_p,
	conf_len, conf_dist, MPI_STENS, 0, MPI_COMM_WORLD);
#endif /* STRESS */
    } else {
      /* forces */
      MPI_Gatherv(forces + firstatom * 3, myatoms, MPI_VECTOR,
	forces, atom_len, atom_dist, MPI_VECTOR, 0, MPI_COMM_WORLD);
      /* energies */
      MPI_Gatherv(forces + energy_p + firstconf, myconf, MPI_DOUBLE,
	forces + energy_p, conf_len, conf_dist, MPI_DOUBLE, 0, MPI_COMM_WORLD);
#ifdef STRESS
      /* stresses */
      MPI_Gatherv(forces + stress_p + 6 * firstconf, myconf, MPI_STENS,
	forces + stress_p, conf_len, conf_dist, MPI_STENS, 0, MPI_COMM_WORLD);
#endif /* STRESS */
    }
#else
    /* Set tmpsum to sum - only matters when not running MPI */
    sum = tmpsum;
#endif /* MPI */

    /* Root process only */
    if (myid == 0) {
      /* Increment function calls */
      fcalls++;
      /* If total sum is NAN return large number instead */
      if (isnan(sum)) {
#ifdef DEBUG
	printf("\n--> Force is nan! <--\n\n");
#endif /* DEBUG */
	return 10e10;
      } else
	return sum;
    }
  }				/* END OF INFINITE LOOP */

  /* Kill off other procs */
  return -1.0;
}

#endif /* ANG && COULOMB */
