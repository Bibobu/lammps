/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
   ------------------------------------------------------------------------- */

/*------------------------------------------------------------------------
  Contributing Authors : Germain Clavier (UCA)
  --------------------------------------------------------------------------*/

#include "compute_born.h"
#include <mpi.h>
#include <cmath>
#include <cstring>

#include "atom.h"
#include "atom_vec.h"
#include "update.h"
#include "domain.h"
#include "neighbor.h"
#include "force.h"
#include "pair.h"
#include "bond.h"
#include "angle.h"
#include "dihedral.h"
#include "improper.h"
#include "molecule.h"
#include "neigh_request.h"
#include "neigh_list.h"
#include "error.h"
#include "memory.h"

using namespace LAMMPS_NS;

#define BIG 1000000000


/* ---------------------------------------------------------------------- */

ComputeBorn::ComputeBorn(LAMMPS *lmp, int narg, char **arg) :
  Compute(lmp, narg, arg)
{
  MPI_Comm_rank(world,&me);

  // For now the matrix can be computed as a 21 element vector

  nvalues = 21;

  // Error check
  // 3D only

  if (domain->dimension < 3)
    error->all(FLERR, "Compute born incompatible with simulation dimension");

  // orthogonal simulation box
  if (domain->triclinic != 0)
    error->all(FLERR, "Compute born incompatible with triclinic simulation box");

  // Initialize some variables

  values_local = values_global = vector = NULL;

  // this fix produces a global vector

  memory->create(vector,nvalues,"born:vector");
  memory->create(values_local,nvalues,"born:values_local");
  memory->create(values_global,nvalues,"born:values_global");
  size_vector = nvalues;

  vector_flag = 1;
  extvector = 0;

}

/* ---------------------------------------------------------------------- */

ComputeBorn::~ComputeBorn()
{

 // delete [] which;

  memory->destroy(values_local);
  memory->destroy(values_global);
  memory->destroy(vector);
}

/* ---------------------------------------------------------------------- */

void ComputeBorn::init()
{

  // Timestep Value
  dt = update->dt;

  pairflag = 0;
  bondflag = 0;
  angleflag = 0;
  dihedflag = 0;
  impflag = 0;

  // Error check

  // This compute requires at least a pair style with pair_born method implemented

  if (force->pair == NULL)
    error->all(FLERR,"No pair style is defined for compute born");
  if (force->pair->born_enable == 0) {
    error->all(FLERR,"Pair style does not support compute born");
    pairflag = 0;
  } else {
    pairflag = 1;
  }

  if (force->bond != NULL) {
    if (force->bond->born_enable == 0) {
      error->warning(FLERR, "Bond style does not support compute born");
      bondflag = 0;
    } else {
      bondflag = 1;
    }
  }

  if (force->angle != NULL) {
    if (force->angle->born_enable == 0) {
      error->warning(FLERR, "Angle style does not support compute born");
      angleflag = 0;
    } else {
      angleflag = 1;
    }
  }

  if (force->dihedral != NULL) {
    if (force->dihedral->born_enable == 0) {
      error->warning(FLERR, "Dihedral style does not support compute born");
      dihedflag = 0;
    } else {
      dihedflag = 1;
    }
  }

  if (force->improper != NULL) {
    if (force->improper->born_enable == 0) {
      error->warning(FLERR, "Improper style does not support compute born");
      impflag = 0;
    } else {
      impflag = 1;
    }
  }

  // need an occasional half neighbor list
  int irequest = neighbor->request((void *) this);
  neighbor->requests[irequest]->pair = 0;
  neighbor->requests[irequest]->compute = 1;
  neighbor->requests[irequest]->occasional = 1;
}

/* ---------------------------------------------------------------------- */

void ComputeBorn::init_list(int /* id */, NeighList *ptr)
{
  list = ptr;
}

/* ----------------------------------------------------------------------
   compute output vector
   ------------------------------------------------------------------------- */

void ComputeBorn::compute_vector()
{
  invoked_array = update->ntimestep;

  // zero out arrays for one sample

  int i;
  for (i = 0; i < nvalues; i++) values_local[i] = 0.0;

  // Compute Born contribution on separate procs
  if (pairflag) compute_pairs();

  if (bondflag) compute_bonds();

  if (angleflag) compute_angles();

  if (dihedflag) compute_dihedrals();

  // Even if stated in Voyatzis-2012, improper and dihedrals
  // are not exactly the same in lammps. Atoms order can depend
  // on the forcefield/improper interaction used. As such,
  // writing a general routine to compute improper contribution
  // might be more tricky than expected.
  // if (impflag) compute_impropers();

  // sum Born contributions over all procs
  MPI_Allreduce(values_local,values_global,nvalues,
                MPI_DOUBLE,MPI_SUM,world);

  int m;
  for (m=0; m<nvalues; m++) {
    vector[m] = values_global[m];
  }
}


/*------------------------------------------------------------------------
  compute Born contribution of local proc
  -------------------------------------------------------------------------*/

void ComputeBorn::compute_pairs()

{
  int i,j,m,ii,jj,inum,jnum,itype,jtype;
  double delx,dely,delz;
  double rsq,factor_coul,factor_lj;
  double dupair,du2pair,rinv;
  int *ilist,*jlist,*numneigh,**firstneigh;

  int *type = atom->type;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;
  double *special_coul = force->special_coul;
  double *special_lj = force->special_lj;
  int newton_pair = force->newton_pair;

  // invoke half neighbor list (will copy or build if necessary)
  neighbor->build_one(list);

  inum = list->inum;
  ilist = list->ilist;
  numneigh = list->numneigh;
  firstneigh = list->firstneigh;

  // loop over neighbors of my atoms

  Pair *pair = force->pair;
  double **cutsq = force->pair->cutsq;

  // Declares born values

  int a,b,c,d;
  double xi[3];
  double fi[3];
  double xj[3];
  double rij[3];
  double pair_pref;
  double r2inv;


  m = 0;
  while (m<nvalues) {
      for (ii = 0; ii < inum; ii++) {
        i = ilist[ii];
        if (!(mask[i] & groupbit)) continue;

        xi[0] = atom->x[i][0];
        xi[1] = atom->x[i][1];
        xi[2] = atom->x[i][2];
        itype = type[i];
        jlist = firstneigh[i];
        jnum = numneigh[i];

        for (jj = 0; jj < jnum; jj++) {
          j = jlist[jj];
          factor_lj = special_lj[sbmask(j)];
          factor_coul = special_coul[sbmask(j)];
          j &= NEIGHMASK;

          if (!(mask[j] & groupbit)) continue;

          xj[0] = atom->x[j][0];
          xj[1] = atom->x[j][1];
          xj[2] = atom->x[j][2];
          delx = xi[0] - xj[0];
          dely = xi[1] - xj[1];
          delz = xi[2] - xj[2];
          rij[0] = xj[0]-xi[0];
          rij[1] = xj[1]-xi[1];
          rij[2] = xj[2]-xi[2];
          rsq = delx*delx + dely*dely + delz*delz;
          r2inv = 1.0/rsq;
          jtype = type[j];

          if (rsq >= cutsq[itype][jtype]) continue;

          if (newton_pair || j < nlocal) {
            // Add contribution to Born tensor

            pair->born(i,j,itype,jtype,rsq,factor_coul,factor_lj,dupair,du2pair);
            pair_pref = du2pair - dupair*rinv;

            // See albemunu in compute_born.h for indices order.
            a = 0;
            b = 0;
            c = 0;
            d = 0;
            for (i = 0; i<21; i++) {
              a = albemunu[i][0];
              b = albemunu[i][1];
              c = albemunu[i][2];
              d = albemunu[i][3];
              values_local[m+i] += pair_pref*rij[a]*rij[b]*rij[c]*rij[d]*r2inv;
            }
          }
        }
      }
    m += 21;
  }
}

/* ----------------------------------------------------------------------
   count bonds and compute bond info on this proc
   only count bond once if newton_bond is off
   all atoms in interaction must be in group
   all atoms in interaction must be known to proc
   if bond is deleted or turned off (type <= 0)
   do not count or count contribution
------------------------------------------------------------------------- */

void ComputeBorn::compute_bonds()
{
  int i,m,n,nb,atom1,atom2,imol,iatom,btype,ivar;
  tagint tagprev;
  double dx,dy,dz,rsq;

  double **x = atom->x;
  double **v = atom->v;
  int *type = atom->type;
  tagint *tag = atom->tag;
  int *num_bond = atom->num_bond;
  tagint **bond_atom = atom->bond_atom;
  int **bond_type = atom->bond_type;
  int *mask = atom->mask;

  int *molindex = atom->molindex;
  int *molatom = atom->molatom;
  Molecule **onemols = atom->avec->onemols;

  int nlocal = atom->nlocal;
  int newton_bond = force->newton_bond;
  int molecular = atom->molecular;

  Bond *bond = force->bond;

  int a,b,c,d;
  double rij[3];
  double rinv, r2inv;
  double pair_pref, dupair, du2pair;

  // loop over all atoms and their bonds

  m = 0;
  while (m<nvalues) {

    for (atom1 = 0; atom1 < nlocal; atom1++) {
      if (!(mask[atom1] & groupbit)) continue;

      if (molecular == 1) nb = num_bond[atom1];
      else {
        if (molindex[atom1] < 0) continue;
        imol = molindex[atom1];
        iatom = molatom[atom1];
        nb = onemols[imol]->num_bond[iatom];
      }

      for (i = 0; i < nb; i++) {
        if (molecular == 1) {
          btype = bond_type[atom1][i];
          atom2 = atom->map(bond_atom[atom1][i]);
        } else {
          tagprev = tag[atom1] - iatom - 1;
          btype = onemols[imol]->bond_type[iatom][i];
          atom2 = atom->map(onemols[imol]->bond_atom[iatom][i]+tagprev);
        }

        if (atom2 < 0 || !(mask[atom2] & groupbit)) continue;
        if (newton_bond == 0 && tag[atom1] > tag[atom2]) continue;
        if (btype <= 0) continue;

        dx = x[atom2][0] - x[atom1][0];
        dy = x[atom2][1] - x[atom1][1];
        dz = x[atom2][2] - x[atom1][2];
        domain->minimum_image(dx,dy,dz);
        rsq = dx*dx + dy*dy + dz*dz;
        rij[0] = dx;
        rij[1] = dy;
        rij[2] = dz;
        r2inv = 1.0/rsq;
        rinv = sqrt(r2inv);

        pair_pref = 0.0;
        dupair = 0.0;
        du2pair = 0.0;
        bond->born(btype,rsq,atom1,atom2,dupair,du2pair);
        pair_pref = du2pair - dupair*rinv;

        a = 0;
        b = 0;
        c = 0;
        d = 0;
        for (i = 0; i<21; i++) {
          a = albemunu[i][0];
          b = albemunu[i][1];
          c = albemunu[i][2];
          d = albemunu[i][3];
          values_local[m+i] += pair_pref*rij[a]*rij[b]*rij[c]*rij[d]*r2inv;
        }
      }
    }
    m += 21;
  }
}

/* ----------------------------------------------------------------------
   count angles and compute angle info on this proc
   only count if 2nd atom is the one storing the angle
   all atoms in interaction must be in group
   all atoms in interaction must be known to proc
   if bond is deleted or turned off (type <= 0)
   do not count or count contribution
------------------------------------------------------------------------- */

void ComputeBorn::compute_angles()
{
  int i,m,n,na,atom1,atom2,atom3,imol,iatom,atype,ivar;
  tagint tagprev;
  double delx1,dely1,delz1,delx2,dely2,delz2;
  double rsq1,rsq2,r1,r2,cost;
  double r1r2, r1r2inv;
  double rsq1inv,rsq2inv,r1inv,r2inv,cinv;
  double *ptr;

  double **x = atom->x;
  tagint *tag = atom->tag;
  int *num_angle = atom->num_angle;
  tagint **angle_atom1 = atom->angle_atom1;
  tagint **angle_atom2 = atom->angle_atom2;
  tagint **angle_atom3 = atom->angle_atom3;
  int **angle_type = atom->angle_type;
  int *mask = atom->mask;

  int *molindex = atom->molindex;
  int *molatom = atom->molatom;
  Molecule **onemols = atom->avec->onemols;

  int nlocal = atom->nlocal;
  int molecular = atom->molecular;

  // loop over all atoms and their angles

  Angle *angle = force->angle;

  int a,b,c,d,e,f;
  double duang, du2ang;
  double del1[3], del2[3];
  double dcos[6];
  double d2cos[21];
  double d2lncos[21];

  // Initializing array for intermediate cos derivatives
  // w regard to strain
  for (i = 0; i < 6; i++) {
    dcos[i] = 0;
  }
  for (i = 0; i < 21; i++) {
    d2cos[i] = 0;
    d2lncos[i] = 0;
  }

  m = 0;
  while (m < nvalues) {
    for (atom2 = 0; atom2 < nlocal; atom2++) {
      if (!(mask[atom2] & groupbit)) continue;

      if (molecular == 1) na = num_angle[atom2];
      else {
        if (molindex[atom2] < 0) continue;
        imol = molindex[atom2];
        iatom = molatom[atom2];
        na = onemols[imol]->num_angle[iatom];
      }

      for (i = 0; i < na; i++) {
        if (molecular == 1) {
          if (tag[atom2] != angle_atom2[atom2][i]) continue;
          atype = angle_type[atom2][i];
          atom1 = atom->map(angle_atom1[atom2][i]);
          atom3 = atom->map(angle_atom3[atom2][i]);
        } else {
          if (tag[atom2] != onemols[imol]->angle_atom2[atom2][i]) continue;
          atype = onemols[imol]->angle_type[atom2][i];
          tagprev = tag[atom2] - iatom - 1;
          atom1 = atom->map(onemols[imol]->angle_atom1[atom2][i]+tagprev);
          atom3 = atom->map(onemols[imol]->angle_atom3[atom2][i]+tagprev);
        }

        if (atom1 < 0 || !(mask[atom1] & groupbit)) continue;
        if (atom3 < 0 || !(mask[atom3] & groupbit)) continue;
        if (atype <= 0) continue;

        delx1 = x[atom1][0] - x[atom2][0];
        dely1 = x[atom1][1] - x[atom2][1];
        delz1 = x[atom1][2] - x[atom2][2];
        domain->minimum_image(delx1,dely1,delz1);
        del1[0] = delx1;
        del1[1] = dely1;
        del1[2] = delz1;

        rsq1 = delx1*delx1 + dely1*dely1 + delz1*delz1;
        rsq1inv = 1.0/rsq1;
        r1 = sqrt(rsq1);
        r1inv = 1.0/r1;

        delx2 = x[atom3][0] - x[atom2][0];
        dely2 = x[atom3][1] - x[atom2][1];
        delz2 = x[atom3][2] - x[atom2][2];
        domain->minimum_image(delx2,dely2,delz2);
        del2[0] = delx2;
        del2[1] = dely2;
        del2[2] = delz2;

        rsq2 = delx2*delx2 + dely2*dely2 + delz2*delz2;
        rsq2inv = 1.0/rsq2;
        r2 = sqrt(rsq2);
        r2inv = 1.0/r2;

        r1r2 = delx1*delx2 + dely1*dely2 + delz1*delz2;
        r1r2inv = 1/r1r2;
        // cost = cosine of angle

        cost = delx1*delx2 + dely1*dely2 + delz1*delz2;
        cost /= r1*r2;
        if (cost > 1.0) cost = 1.0;
        if (cost < -1.0) cost = -1.0;
        cinv = 1.0/cost;

        // The method must return derivative with regards
        // to cos(theta)!
        // Use the chain rule if needed:
        // dU(t)/de = dt/dcos(t)*dU(t)/dt*dcos(t)/de
        // with dt/dcos(t) = -1/sin(t)
        angle->born(atype,atom1,atom2,atom3,duang,du2ang);

        // Voigt notation
        // 1 = 11, 2 = 22, 3 = 33
        // 4 = 23, 5 = 13, 6 = 12
        a = 0;
        b = 0;
        c = 0;
        d = 0;
        for (i = 0; i<6; i++) {
          a = albe[i][0];
          b = albe[i][1];
          dcos[i] = cost*(del1[a]*del2[b]+del1[b]*del2[a]*r1r2inv -
                          del1[a]*del1[b]*rsq1inv - del2[a]*del2[b]*rsq2inv);
        }
        for (i = 0; i<21; i++) {
          a = albemunu[i][0];
          b = albemunu[i][1];
          c = albemunu[i][2];
          d = albemunu[i][3];
          e = albe[i][0];
          f = albe[i][1];
          d2lncos[i] = 2*(del1[a]*del1[b]*del1[c]*del1[d]*rsq1inv*rsq1inv +
                          del2[a]*del2[b]*del2[c]*del2[d]*rsq2inv*rsq2inv) -
                         (del1[a]*del2[b]+del1[b]*del2[a]) *
                         (del1[c]*del2[d]+del1[d]*del2[c]) *
                         r1r2inv*r1r2inv;
          d2cos[i] =  cost*d2lncos[i] + dcos[e]*dcos[f]*cinv;
          values_local[m+i] += duang*d2cos[i] + du2ang*dcos[e]*dcos[f];
        }
      }
    }
  m+=21;
  }
}

/* ----------------------------------------------------------------------
   count dihedrals on this proc
   only count if 2nd atom is the one storing the dihedral
   all atoms in interaction must be in group
   all atoms in interaction must be known to proc
   if flag is set, compute requested info about dihedral
------------------------------------------------------------------------- */

void ComputeBorn::compute_dihedrals()
{
  int i,m,n,nd,atom1,atom2,atom3,atom4,imol,iatom,dtype,ivar;
  tagint tagprev;
  double vb1x,vb1y,vb1z,vb2x,vb2y,vb2z,vb3x,vb3y,vb3z,vb2xm,vb2ym,vb2zm;
  double ax,ay,az,bx,by,bz,rasq,rbsq,rgsq,rg,ra2inv,rb2inv,rabinv;
  double si,co,phi;
  double *ptr;

  double **x = atom->x;
  tagint *tag = atom->tag;
  int *num_dihedral = atom->num_dihedral;
  tagint **dihedral_atom1 = atom->dihedral_atom1;
  tagint **dihedral_atom2 = atom->dihedral_atom2;
  tagint **dihedral_atom3 = atom->dihedral_atom3;
  tagint **dihedral_atom4 = atom->dihedral_atom4;
  int *mask = atom->mask;

  int *molindex = atom->molindex;
  int *molatom = atom->molatom;
  Molecule **onemols = atom->avec->onemols;

  int nlocal = atom->nlocal;
  int molecular = atom->molecular;

  // loop over all atoms and their dihedrals

  Dihedral *dihedral = force->dihedral;

  double dudih,du2dih;
  int a,b,c,d,e,f;
  double b1sq;
  double b2sq;
  double b3sq;
  double b1b2;
  double b1b3;
  double b2b3;
  double b1[3];
  double b2[3];
  double b3[3];

  // Actually derivatives of the square of the
  // vectors dot product.
  double dmn[6];
  double dmm[6];
  double dnn[6];
  double d2mn[21];
  double d2mm[21];
  double d2nn[21];

  double dcos[6];
  double d2cos[21];

  for (i = 0; i < 6; i++) {
    dmn[i] =0;
    dmm[i] = 0;
    dnn[i] = 0;
    dcos[i] = 0;
  }
  for (i = 0; i < 21; i++) {
    d2mn[i] = 0;
    d2mm[i] = 0;
    d2nn[i] = 0;
    d2cos[i] = 0;
  }

  m = 0;
  while (m < nvalues) {
    for (atom2 = 0; atom2 < nlocal; atom2++) {
      if (!(mask[atom2] & groupbit)) continue;

      if (molecular == 1) nd = num_dihedral[atom2];
      else {
        if (molindex[atom2] < 0) continue;
        imol = molindex[atom2];
        iatom = molatom[atom2];
        nd = onemols[imol]->num_dihedral[iatom];
      }

      for (i = 0; i < nd; i++) {
        if (molecular == 1) {
          if (tag[atom2] != dihedral_atom2[atom2][i]) continue;
          atom1 = atom->map(dihedral_atom1[atom2][i]);
          atom3 = atom->map(dihedral_atom3[atom2][i]);
          atom4 = atom->map(dihedral_atom4[atom2][i]);
        } else {
          if (tag[atom2] != onemols[imol]->dihedral_atom2[atom2][i]) continue;
          tagprev = tag[atom2] - iatom - 1;
          atom1 = atom->map(onemols[imol]->dihedral_atom1[atom2][i]+tagprev);
          atom3 = atom->map(onemols[imol]->dihedral_atom3[atom2][i]+tagprev);
          atom4 = atom->map(onemols[imol]->dihedral_atom4[atom2][i]+tagprev);
        }

        if (atom1 < 0 || !(mask[atom1] & groupbit)) continue;
        if (atom3 < 0 || !(mask[atom3] & groupbit)) continue;
        if (atom4 < 0 || !(mask[atom4] & groupbit)) continue;

        // phi calculation from dihedral style harmonic

        // The method must return derivative with regards
        // to cos(phi)!
        // Use the chain rule if needed:
        // dU(t)/de = dt/dcos(t)*dU(t)/dt*dcos(t)/de
        // with dt/dcos(t) = -1/sin(t)

        dihedral->born(nd,atom1,atom2,atom3,atom4,dudih,du2dih);

        vb1x = x[atom1][0] - x[atom2][0];
        vb1y = x[atom1][1] - x[atom2][1];
        vb1z = x[atom1][2] - x[atom2][2];
        domain->minimum_image(vb1x,vb1y,vb1z);
        b1[0] = vb1x;
        b1[1] = vb1y;
        b1[2] = vb1z;
        b1sq = b1[0]*b1[0]+b1[1]*b1[1]+b1[2]*b1[2];

        vb2x = x[atom3][0] - x[atom2][0];
        vb2y = x[atom3][1] - x[atom2][1];
        vb2z = x[atom3][2] - x[atom2][2];
        domain->minimum_image(vb2x,vb2y,vb2z);
        b2[0] = vb2x;
        b2[1] = vb2y;
        b2[2] = vb2z;
        b2sq = b2[0]*b2[0]+b2[1]*b2[1]+b2[2]*b2[2];

        vb2xm = -vb2x;
        vb2ym = -vb2y;
        vb2zm = -vb2z;
        domain->minimum_image(vb2xm,vb2ym,vb2zm);

        vb3x = x[atom4][0] - x[atom3][0];
        vb3y = x[atom4][1] - x[atom3][1];
        vb3z = x[atom4][2] - x[atom3][2];
        domain->minimum_image(vb3x,vb3y,vb3z);
        b3[0] = vb3x;
        b3[1] = vb3y;
        b3[2] = vb3z;
        b3sq = b3[0]*b3[0]+b3[1]*b3[1]+b3[2]*b3[2];

        b1b2 = b1[0]*b2[0]+b1[1]*b2[1]+b1[2]*b2[2];
        b1b3 = b1[0]*b3[0]+b1[1]*b3[1]+b1[2]*b3[2];
        b2b3 = b2[0]*b3[0]+b2[1]*b3[1]+b2[2]*b3[2];

        ax = vb1y*vb2zm - vb1z*vb2ym;
        ay = vb1z*vb2xm - vb1x*vb2zm;
        az = vb1x*vb2ym - vb1y*vb2xm;
        bx = vb3y*vb2zm - vb3z*vb2ym;
        by = vb3z*vb2xm - vb3x*vb2zm;
        bz = vb3x*vb2ym - vb3y*vb2xm;

        rasq = ax*ax + ay*ay + az*az;
        rbsq = bx*bx + by*by + bz*bz;
        rgsq = vb2xm*vb2xm + vb2ym*vb2ym + vb2zm*vb2zm;
        rg = sqrt(rgsq);

        ra2inv = rb2inv = 0.0;
        if (rasq > 0) ra2inv = 1.0/rasq;
        if (rbsq > 0) rb2inv = 1.0/rbsq;
        rabinv = sqrt(ra2inv*rb2inv);

        co = (ax*bx + ay*by + az*bz)*rabinv;
        si = rg*rabinv*(ax*vb3x + ay*vb3y + az*vb3z);

        if (co > 1.0) co = 1.0;
        if (co < -1.0) co = -1.0;
        phi = atan2(si,co);

        // above a and b are m and n vectors
        // here they are integers indices
        a = 0;
        b = 0;
        c = 0;
        d = 0;
        e = 0;
        f = 0;
        for (i = 0; i<6; i++) {
          a = albe[i][0];
          b = albe[i][1];
          dmm[i] = 2*(b2sq*b1[a]*b1[b]+b1sq*b2[a]*b2[b] - b1b2*(b1[a]*b2[b]+b1[b]*b2[a]));
          dnn[i] = 2*(b3sq*b2[a]*b2[b]+b2sq*b3[a]*b3[b] - b2b3*(b2[a]*b3[b]+b2[b]*b3[a]));
          dmn[i] = b1b2*(b2[a]*b3[b]+b2[b]*b3[a]) + b2b3*(b1[a]*b2[b]+b1[b]*b2[a])
                 - 2*(b1b3*b2[a]*b2[b]) - b2sq*(b1[a]*b3[b]+b1[b]*b3[a]);
          dcos[i] = co*(rabinv*rabinv*dmn[i] - ra2inv*dmm[i] - rb2inv*dnn[i])/2.;
        }
        for (i = 0; i<21; i++) {
          a = albemunu[i][0];
          b = albemunu[i][1];
          c = albemunu[i][2];
          d = albemunu[i][3];
          e = albe[i][0];
          f = albe[i][1];
          d2mm[i] = 4*(b1[a]*b1[b]*b2[c]*b2[d] + b1[c]*b1[d]*b2[a]*b2[b])
                  - 8*(b1[a]*b2[b]+b1[b]*b2[a])*(b1[c]*b2[d]+b1[d]*b2[c]);
          d2nn[i] = 4*(b2[a]*b2[b]*b3[c]*b3[d] + b2[c]*b2[d]*b3[a]*b3[b])
                  - 8*(b2[a]*b3[b]+b2[b]*b3[a])*(b2[c]*b3[d]+b2[d]*b3[c]);
          d2mn[i] = (b1[a]*b2[b]+b1[b]*b2[a])*(b2[c]*b3[d]+b2[d]*b3[c])
                  + (b2[a]*b3[b]+b2[b]*b3[a])*(b1[c]*b2[d]+b1[d]*b2[d])
                  - (b1[a]*b3[b]+b1[b]*b3[a])*(b2[c]*b2[d]+b2[c]*b2[d])
                  - (b1[c]*b3[d]+b1[d]*b3[c])*(b2[a]*b2[b]+b2[a]*b2[b]);
          d2cos[i] = co/2.*(
                  rabinv*rabinv*d2mn[i]
                  - rabinv*rabinv*rabinv*rabinv*dmn[e]*dmn[f]
                  + ra2inv*ra2inv*dmm[e]*dmm[f]
                  - ra2inv*d2mm[i]
                  + rb2inv*rb2inv*dnn[e]*dnn[f]
                  - rb2inv*d2nn[i] );
          values_local[m+i] += dudih*d2cos[i] + du2dih*dcos[e]*dcos[f];
        }
      }
    }
  m+=21;
  }
}