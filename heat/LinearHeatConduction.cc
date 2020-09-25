#include "LinearHeatConduction.h"

/*
 Authors: Niels Aage, Erik Andreassen, Boyan Lazarov, August 2013

 Disclaimer:
 The authors reserves all rights but does not guaranty that the code is
 free from errors. Furthermore, we shall not be liable in any event
 caused by the use of the program.
 */

/*
 * Modified by Zhidong Brian Zhang in August 2020, University of Waterloo
 */

LinearHeatConduction::LinearHeatConduction (DM da_nodes, DM da_elem,
    PetscInt numLoads, Vec xPassive0, Vec xPassive1, Vec xPassive2) {
  // Set pointers to null
  K = NULL;
  U = NULL;
  RHS = NULL;
  N = NULL;
  ksp = NULL;
  da_nodal = NULL;

  // Parameters - to be changed on read of variables
  nlvls = 4;
  PetscBool flg;
  PetscOptionsGetInt (NULL, NULL, "-nlvls", &nlvls, &flg);

  // Setup heat conductivity matrix, heat load vector and bcs (Dirichlet) for the design
  // problem
  SetUpLoadAndBC (da_nodes, da_elem, xPassive0, xPassive1, xPassive2);
}

LinearHeatConduction::~LinearHeatConduction () {
  // Deallocate
  VecDestroy (&(U));
  VecDestroy (&(RHS));
  VecDestroy (&(N));
  MatDestroy (&(K));
  KSPDestroy (&(ksp));

  if (da_nodal != NULL) {
    DMDestroy (&(da_nodal));
  }
}

PetscErrorCode LinearHeatConduction::SetUpLoadAndBC (DM da_nodes, DM da_elem,
    Vec xPassive0, Vec xPassive1, Vec xPassive2) {
  PetscErrorCode ierr = 0;

#if  DIM == 2
  // Extract information from input DM and create one for the linear elasticity
  // number of nodal dofs: (u,v)
  PetscInt numnodaldof = 1; // new

  // Stencil width: each node connects to a box around it - linear elements
  PetscInt stencilwidth = 1;

  PetscScalar dx, dy;
  DMBoundaryType bx, by;
  DMDAStencilType stype;
  {
    // Extract information from the nodal mesh
    PetscInt M, N, md, nd;
    DMDAGetInfo (da_nodes, NULL, &M, &N, NULL, &md, &nd, NULL, NULL, NULL, &bx,
        &by, NULL, &stype);

    // Find the element size
    Vec lcoor;
    DMGetCoordinatesLocal (da_nodes, &lcoor);
    PetscScalar *lcoorp;
    VecGetArray (lcoor, &lcoorp);

    PetscInt nel, nen;
    const PetscInt *necon;
    DMDAGetElements_2D (da_nodes, &nel, &nen, &necon);

    // Use the first element to compute the dx, dy, dz
    dx = lcoorp[DIM * necon[0 * nen + 1] + 0] - lcoorp[DIM * necon[0 * nen + 0] + 0];
    dy = lcoorp[DIM * necon[0 * nen + 2] + 1] - lcoorp[DIM * necon[0 * nen + 1] + 1];
    VecRestoreArray (lcoor, &lcoorp);

    nn[0] = M;
    nn[1] = N;

    ne[0] = nn[0] - 1;
    ne[1] = nn[1] - 1;

    xc[0] = 0.0;
    xc[1] = ne[0] * dx;
    xc[2] = 0.0;
    xc[3] = ne[1] * dy;
  }

  // Create the nodal mesh
  DMDACreate2d (PETSC_COMM_WORLD, bx, by, stype, nn[0], nn[1], PETSC_DECIDE,
  PETSC_DECIDE, numnodaldof, stencilwidth, 0, 0, &(da_nodal));
  // Initialize
  DMSetFromOptions (da_nodal);
  DMSetUp (da_nodal);

  // Set the coordinates
  DMDASetUniformCoordinates (da_nodal, xc[0], xc[1], xc[2], xc[3], 0.0, 0.0);
  // Set the element type to Q1: Otherwise calls to GetElements will change to
  // P1 ! STILL DOESN*T WORK !!!!
  DMDASetElementType (da_nodal, DMDA_ELEMENT_Q1);

  // Allocate matrix and the RHS and Solution vector and Dirichlet vector
  ierr = DMCreateMatrix (da_nodal, &(K));
  CHKERRQ(ierr);
  ierr = DMCreateGlobalVector (da_nodal, &(U));
  CHKERRQ(ierr);
  VecDuplicate (U, &(RHS));
  VecDuplicate (U, &(N));

  // Set the local heat conductivity matrix
  PetscScalar X[4] = { 0.0, dx, dx, 0.0 };
  PetscScalar Y[4] = { 0.0, 0.0, dy, dy };

  // Compute the element stiffnes matrix - constant due to structured grid
  Quad4Isoparametric (X, Y, false, KE);

  // Set the RHS and Dirichlet vector
  VecSet (N, 1.0);
  VecSet (RHS, 0.0);

  // Global coordinates and a pointer
  Vec lcoor; // borrowed ref - do not destroy!
  PetscScalar *lcoorp;

  // Get local coordinates in local node numbering including ghosts
  ierr = DMGetCoordinatesLocal (da_nodal, &lcoor);
  CHKERRQ(ierr);
  VecGetArray (lcoor, &lcoorp);

  // Get local dof number
  PetscInt nn;
  VecGetSize (lcoor, &nn);

  // Compute epsilon parameter for finding points in space:
  PetscScalar epsi = PetscMin(dx * 0.05, dy * 0.05);
  // Passive design variable vector
  PetscScalar *xPassive0p, *xPassive1p, *xPassive2p;
  VecGetArray (xPassive0, &xPassive0p);
  VecGetArray (xPassive1, &xPassive1p);
  VecGetArray (xPassive2, &xPassive2p);

  // Set the RHS and Dirichlet vector
  VecSet (N, 1.0);
  VecSet (RHS, 0.0);
  PetscScalar rhs_ele[4]; // local rhs
  PetscScalar n_ele[4]; // local n
  PetscInt edof[4];

  // Global coordinates of elements and a pointer
  Vec elcoor; // borrowed ref - do not destroy!
  PetscScalar *elcoorp;

  // Get local coordinates in local element numbering including ghosts
  ierr = DMGetCoordinatesLocal (da_elem, &elcoor);
  CHKERRQ(ierr);
  VecGetArray (elcoor, &elcoorp);

  // Find the element size
  PetscInt nel, nen;
  const PetscInt *necon;
  DMDAGetElements_2D (da_nodes, &nel, &nen, &necon);

  if (IMPORT_GEO == 0) {
    // Set the values:
    // In this case: N = the wall at (1/4 * 1/4) of the bottom is clamped,
    //               RHS(z) = 0.001 within the whole domain;
    PetscScalar LoadIntensity = 0.001;
    for (PetscInt i = 0; i < nn; i++) {
      // Make an area with all dofs clamped, (1/4 * 1/4) of the bottom
      if (i % 2 == 0 && PetscAbsScalar (lcoorp[i + 1] - xc[2]) < epsi && (lcoorp[i] >= xc[1] / 8.0 * 3 && lcoorp[i] <= xc[1] / 8.0 * 5)) {
        VecSetValueLocal (N, i / 2, 0.0, INSERT_VALUES);
      }
    }

//    for (PetscInt i = 0; i < nn; i++) {
//          // Make an area with all dofs clamped, (1/4 * 1/4) of the bottom
//          if (i % 2 == 0 && PetscAbsScalar (lcoorp[i + 1] - xc[3]) < epsi && PetscAbsScalar (lcoorp[i] - xc[1]) < epsi ) {
//            VecSetValueLocal (RHS, i / 2, LoadIntensity, INSERT_VALUES);
//          }
//        }
    // Every point has a thermal load except the clamped area
    // Since the heat load is a body load, need to loop over elements
    // So that don't need to adjust the boundaries and the corners
    for (PetscInt i = 0; i < nel; i++) {
      memset (rhs_ele, 0.0, sizeof(rhs_ele[0]) * 4);

      // Global dof in the RHS vector
      for (PetscInt l = 0; l < nen; l++) {
        edof[l] = necon[i * nen + l]; // dof in globe
      }
      for (PetscInt j = 0; j < 4; j++) {
        rhs_ele[j] = 1.0 / 4 * LoadIntensity;
      }
      ierr = VecSetValuesLocal (RHS, 4, edof, rhs_ele, ADD_VALUES);
      CHKERRQ(ierr);
    }

  } else {
    // Set the values:
    // In this case:
    // xPassive1 indicates fix,
    // xPassive2 indicates loading.
    // Load and constraints
    PetscScalar LoadIntensity = 0.001;
    for (PetscInt i = 0; i < nel; i++) {
      memset (rhs_ele, 0.0, sizeof(rhs_ele[0]) * 4);
      memset (n_ele, 0.0, sizeof(n_ele[0]) * 4);

      // Global dof in the RHS vector
      for (PetscInt l = 0; l < nen; l++) {
        edof[l] = necon[i * nen + l]; // dof in globe
      }
      if (xPassive0p[i] == 0) {
        for (PetscInt j = 0; j < 4; j++) {
          rhs_ele[j] = LoadIntensity;
        }
        ierr = VecSetValuesLocal (RHS, 4, edof, rhs_ele, ADD_VALUES);
        CHKERRQ(ierr);
      }
      if (xPassive1p[i] == 1) {
        for (PetscInt j = 0; j < 4; j++) {
          n_ele[j] = 0.0;
        }
        ierr = VecSetValuesLocal (N, 4, edof, n_ele, INSERT_VALUES);
        CHKERRQ(ierr);
      }
    }
  }
#endif

#if DIM == 3
  // Extract information from input DM and create one for the linear elasticity
  // number of nodal dofs: (u,v,w)
  PetscInt numnodaldof = 1; // new

  // Stencil width: each node connects to a box around it - linear elements
  PetscInt stencilwidth = 1;

  PetscScalar dx, dy, dz;
  DMBoundaryType bx, by, bz;
  DMDAStencilType stype;
  {
    // Extract information from the nodal mesh
    PetscInt M, N, P, md, nd, pd;
    DMDAGetInfo (da_nodes,
    NULL, &M, &N, &P, &md, &nd, &pd,
    NULL,
    NULL, &bx, &by, &bz, &stype);

    // Find the element size
    Vec lcoor;
    DMGetCoordinatesLocal (da_nodes, &lcoor);
    PetscScalar *lcoorp;
    VecGetArray (lcoor, &lcoorp);

    PetscInt nel, nen;
    const PetscInt *necon;
    DMDAGetElements_3D (da_nodes, &nel, &nen, &necon);

    // Use the first element to compute the dx, dy, dz
    dx = lcoorp[3 * necon[0 * nen + 1] + 0]
        - lcoorp[3 * necon[0 * nen + 0] + 0];
    dy = lcoorp[3 * necon[0 * nen + 2] + 1]
        - lcoorp[3 * necon[0 * nen + 1] + 1];
    dz = lcoorp[3 * necon[0 * nen + 4] + 2]
        - lcoorp[3 * necon[0 * nen + 0] + 2];
    VecRestoreArray (lcoor, &lcoorp);

    nn[0] = M;
    nn[1] = N;
    nn[2] = P;

    ne[0] = nn[0] - 1;
    ne[1] = nn[1] - 1;
    ne[2] = nn[2] - 1;

    xc[0] = 0.0;
    xc[1] = ne[0] * dx;
    xc[2] = 0.0;
    xc[3] = ne[1] * dy;
    xc[4] = 0.0;
    xc[5] = ne[2] * dz;
  }

  // Create the nodal mesh
  DMDACreate3d (PETSC_COMM_WORLD, bx, by, bz, stype, nn[0], nn[1], nn[2],
  PETSC_DECIDE, PETSC_DECIDE, PETSC_DECIDE, numnodaldof, stencilwidth, 0, 0, 0,
      &(da_nodal));
  // Initialize
  DMSetFromOptions (da_nodal);
  DMSetUp (da_nodal);

  // Set the coordinates
  DMDASetUniformCoordinates (da_nodal, xc[0], xc[1], xc[2], xc[3], xc[4],
      xc[5]);
  // Set the element type to Q1: Otherwise calls to GetElements will change to
  // P1 ! STILL DOESN*T WORK !!!!
  DMDASetElementType (da_nodal, DMDA_ELEMENT_Q1);
//  DMDASetElementType (da_nodal, DMDA_ELEMENT_Q1);

  // Allocate matrix and the RHS and Solution vector and Dirichlet vector
  ierr = DMCreateMatrix (da_nodal, &(K));
  CHKERRQ(ierr);
  ierr = DMCreateGlobalVector (da_nodal, &(U));
  CHKERRQ(ierr);
  VecDuplicate (U, &(RHS));
  VecDuplicate (U, &(N));

  // Set the local heat conductivity matrix
  PetscScalar X[8] = { 0.0, dx, dx, 0.0, 0.0, dx, dx, 0.0 };
  PetscScalar Y[8] = { 0.0, 0.0, dy, dy, 0.0, 0.0, dy, dy };
  PetscScalar Z[8] = { 0.0, 0.0, 0.0, 0.0, dz, dz, dz, dz };

  // Compute the element heat conductivity matrix - constant due to structured grid
  Hex8Isoparametric (X, Y, Z, false, KE);

  // Set the RHS and Dirichlet vector
  VecSet (N, 1.0);
  VecSet (RHS, 0.0);

  // Global coordinates and a pointer
  Vec lcoor; // borrowed ref - do not destroy!
  PetscScalar *lcoorp;

  // Get local coordinates in local node numbering including ghosts
  ierr = DMGetCoordinatesLocal (da_nodal, &lcoor);
  CHKERRQ(ierr);
  VecGetArray (lcoor, &lcoorp);

  // Get local dof number
  PetscInt nn;
  VecGetSize (lcoor, &nn);

  // Compute epsilon parameter for finding points in space:
  PetscScalar epsi = PetscMin(dx * 0.05, PetscMin(dy * 0.05, dz * 0.05));

  // Passive design variable vector
  PetscScalar *xPassive0p, *xPassive1p, *xPassive2p;
  VecGetArray (xPassive0, &xPassive0p);
  VecGetArray (xPassive1, &xPassive1p);
  VecGetArray (xPassive2, &xPassive2p);

  // Set the RHS and Dirichlet vector
  VecSet (N, 1.0);
  VecSet (RHS, 0.0);
  PetscScalar rhs_ele[8]; // local rhs
  PetscScalar n_ele[8]; // local n
  PetscInt edof[8];

  // Global coordinates of elements and a pointer
  Vec elcoor; // borrowed ref - do not destroy!
  PetscScalar *elcoorp;

  // Get local coordinates in local element numbering including ghosts
  ierr = DMGetCoordinatesLocal (da_elem, &elcoor);
  CHKERRQ(ierr);
  VecGetArray (elcoor, &elcoorp);

  // Find the element size
  PetscInt nel, nen;
  const PetscInt *necon;
  DMDAGetElements_3D (da_nodes, &nel, &nen, &necon);

  if (IMPORT_GEO == 0) {
    // Set the values:
    // In this case: N = the wall at (1/4 * 1/4) of the bottom is clamped,
    //               RHS(z) = 0.001 within the whole domain;
    PetscScalar LoadIntensity = 0.001;
    for (PetscInt i = 0; i < nn; i++) {
      // Make an area with all dofs clamped, (1/4 * 1/4) of the
      if (i % 3 == 0 && PetscAbsScalar (lcoorp[i + 1] - xc[2]) < epsi
          && (lcoorp[i] >= xc[1] / 8.0 * 3 && lcoorp[i] <= xc[1] / 8.0 * 5)
          && (lcoorp[i + 2] >= xc[5] / 8.0 * 3 && lcoorp[i + 2]
              <= xc[5] / 8.0 * 5)) {
        VecSetValueLocal (N, i / 3, 0.0, INSERT_VALUES);
      }
    }

    // Every point has a thermal load except the clamped area
    // Since the heat load is a body load, need to loop over elements
    // So that don't need to adjust the boundaries and the corners
    for (PetscInt i = 0; i < nel; i++) {
      memset (rhs_ele, 0.0, sizeof(rhs_ele[0]) * 8);
      memset (n_ele, 0.0, sizeof(n_ele[0]) * 8);

      // Global dof in the RHS vector
      for (PetscInt l = 0; l < nen; l++) {
        edof[l] = necon[i * nen + l]; // dof in globe
      }
      for (PetscInt j = 0; j < 8; j++) {
        rhs_ele[j] = 1.0 / 8 * LoadIntensity;
      }
      ierr = VecSetValuesLocal (RHS, 8, edof, rhs_ele, ADD_VALUES);
      CHKERRQ(ierr);
    }

  } else {
    // Set the values:
    // In this case:
    // xPassive1 indicates fix,
    // xPassive2 indicates loading.
    // Load and constraints
    PetscScalar LoadIntensity = 0.001;
    for (PetscInt i = 0; i < nel; i++) {
      memset (rhs_ele, 0.0, sizeof(rhs_ele[0]) * 8);
      memset (n_ele, 0.0, sizeof(n_ele[0]) * 8);

      // Global dof in the RHS vector
      for (PetscInt l = 0; l < nen; l++) {
        edof[l] = necon[i * nen + l]; // dof in globe
      }
      if (xPassive0p[i] == 0) {
        for (PetscInt j = 0; j < 8; j++) {
          rhs_ele[j] = LoadIntensity;
        }
        ierr = VecSetValuesLocal (RHS, 8, edof, rhs_ele, ADD_VALUES);
        CHKERRQ(ierr);
      }
      if (xPassive1p[i] == 1) {
        for (PetscInt j = 0; j < 8; j++) {
          n_ele[j] = 0.0;
        }
        ierr = VecSetValuesLocal (N, 8, edof, n_ele, INSERT_VALUES);
        CHKERRQ(ierr);
      }
    }
  }

#endif

  // Restore vectors
  VecAssemblyBegin (N);
  VecAssemblyEnd (N);
  VecAssemblyBegin (RHS);
  VecAssemblyEnd (RHS);
  VecRestoreArray (lcoor, &lcoorp);
  VecRestoreArray (elcoor, &elcoorp);
  DMDARestoreElements (da_nodes, &nel, &nen, &necon);
  VecRestoreArray (xPassive0, &xPassive0p);
  VecRestoreArray (xPassive1, &xPassive1p);
  VecRestoreArray (xPassive2, &xPassive2p);

  return ierr;
}

PetscErrorCode LinearHeatConduction::SolveState (Vec xPhys, PetscScalar Emin,
    PetscScalar Emax, PetscScalar penal) {

  PetscErrorCode ierr;

  double t1, t2;
  t1 = MPI_Wtime ();

  // Assemble the heat conductivity matrix
  ierr = AssembleConductivityMatrix (xPhys, Emin, Emax, penal);
  CHKERRQ(ierr);

  // Setup the solver
  if (ksp == NULL) {
    ierr = SetUpSolver ();
    CHKERRQ(ierr);
  } else {
    ierr = KSPSetOperators (ksp, K, K);
    CHKERRQ(ierr);
    KSPSetUp (ksp);
  }

  // Solve
  ierr = KSPSolve (ksp, RHS, U);
  CHKERRQ(ierr);
  CHKERRQ(ierr);

  // DEBUG
  // Get iteration number and residual from KSP
  PetscInt niter;
  PetscScalar rnorm;
  KSPGetIterationNumber (ksp, &niter);
  KSPGetResidualNorm (ksp, &rnorm);
  PetscReal RHSnorm;
  ierr = VecNorm (RHS, NORM_2, &RHSnorm);
  CHKERRQ(ierr);
  rnorm = rnorm / RHSnorm;

  t2 = MPI_Wtime ();
  PetscPrintf (PETSC_COMM_WORLD,
      "State solver:  iter: %i, rerr.: %e, time: %f\n", niter, rnorm, t2 - t1);

  return ierr;
}

PetscErrorCode LinearHeatConduction::ComputeObjectiveConstraintsSensitivities (
    PetscScalar *fx, PetscScalar *gx, Vec dfdx, Vec dgdx, Vec xPhys,
    PetscScalar Emin, PetscScalar Emax, PetscScalar penal, PetscScalar volfrac,
    Vec xPassive0, Vec xPassive1, Vec xPassive2) {
  // Errorcode
  PetscErrorCode ierr;

  // Solve state eqs
  ierr = SolveState (xPhys, Emin, Emax, penal);
  CHKERRQ(ierr);

  // Get the FE mesh structure (from the nodal mesh)
  PetscInt nel, nen;
  const PetscInt *necon;
#if DIM == 2
  ierr = DMDAGetElements_2D (da_nodal, &nel, &nen, &necon);
  CHKERRQ(ierr);
#elif DIM == 3
  ierr = DMDAGetElements_3D (da_nodal, &nel, &nen, &necon);
  CHKERRQ(ierr);
#endif
  // DMDAGetElements(da_nodes,&nel,&nen,&necon); // Still issue with elemtype
  // change !

  // Get pointer to the densities
  PetscScalar *xp, *xPassive0p, *xPassive1p, *xPassive2p;
  VecGetArray (xPhys, &xp);
  VecGetArray (xPassive0, &xPassive0p);
  VecGetArray (xPassive1, &xPassive1p);
  VecGetArray (xPassive2, &xPassive2p);

  // Get Solution
  Vec Uloc;
  DMCreateLocalVector (da_nodal, &Uloc);
  DMGlobalToLocalBegin (da_nodal, U, INSERT_VALUES, Uloc);
  DMGlobalToLocalEnd (da_nodal, U, INSERT_VALUES, Uloc);

  // get pointer to local vector
  PetscScalar *up;
  VecGetArray (Uloc, &up);

  // Get dfdx
  PetscScalar *df;
  VecGetArray (dfdx, &df);

  // Edof array, new
  PetscInt edof[nedof];

  fx[0] = 0.0;
  // Loop over elements, new
  for (PetscInt i = 0; i < nel; i++) {
    // loop over element nodes
    if (xPassive0p[i] == 0 && xPassive1p[i] == 0 && xPassive2p[i] == 0) {

      for (PetscInt j = 0; j < nen; j++) {
        // Get local dofs
        edof[j] = necon[i * nen + j];
      }
      // Use SIMP for heat conductivity interpolation
      PetscScalar uKu = 0.0;
      for (PetscInt k = 0; k < nedof; k++) {
        for (PetscInt h = 0; h < nedof; h++) {
          uKu += up[edof[k]] * KE[k * nedof + h] * up[edof[h]];
        }
      }
      // Add to objective
      fx[0] += (Emin + PetscPowScalar(xp[i], penal) * (Emax - Emin)) * uKu;
      // Set the Senstivity
      df[i] = -1.0 * penal * PetscPowScalar(xp[i], penal - 1) * (Emax - Emin)
              * uKu;
    } else if (xPassive0p[i] == 1) {
      df[i] = 1.0E9;
    } else if (xPassive1p[i] == 1 || xPassive2p[i] == 1) {
      df[i] = -1.0E9;
    }
  }

  // Allreduce fx[0]
  PetscScalar tmp = fx[0];
  fx[0] = 0.0;
  MPI_Allreduce(&tmp, &(fx[0]), 1, MPIU_SCALAR, MPI_SUM, PETSC_COMM_WORLD);

  // Get mash vectors to exclude the non design domain from dgdx (no better way?) new newly added
  Vec tmpVec0, tmpVec1, tmpVec2;
  VecDuplicate (dgdx, &tmpVec0);
  VecCopy (xPassive0, tmpVec0);
  VecShift (tmpVec0, -1);
  VecScale (tmpVec0, -1);
  VecDuplicate (dgdx, &tmpVec1);
  VecCopy (xPassive1, tmpVec1);
  VecShift (tmpVec1, -1);
  VecScale (tmpVec1, -1);
  VecDuplicate (dgdx, &tmpVec2);
  VecCopy (xPassive2, tmpVec2);
  VecShift (tmpVec2, -1);
  VecScale (tmpVec2, -1);

  // Get tmp xPhys, excluding all non design domain
  Vec tmpxPhys;
  VecDuplicate (xPhys, &tmpxPhys);
  VecCopy (xPhys, tmpxPhys);
  VecPointwiseMult (tmpxPhys, tmpxPhys, tmpVec0);
  VecPointwiseMult (tmpxPhys, tmpxPhys, tmpVec1);
  VecPointwiseMult (tmpxPhys, tmpxPhys, tmpVec2);

  // Compute volume constraint gx[0]
  PetscScalar nNonDesign0, nNonDesign1, nNonDesign2; // new newly added
  VecSum (xPassive0, &nNonDesign0); // new newly added
  VecSum (xPassive1, &nNonDesign1); // new newly added
  VecSum (xPassive2, &nNonDesign2); // new newly added

  PetscInt neltot;
  VecGetSize (tmpxPhys, &neltot);
  gx[0] = 0;
  VecSum (tmpxPhys, &(gx[0]));
  PetscPrintf (PETSC_COMM_WORLD, "non designable volume: %f\n",
      nNonDesign0 + nNonDesign1 + nNonDesign2);
  PetscPrintf (PETSC_COMM_WORLD, "volume: %f\n", gx[0]);
  gx[0] = gx[0]
      / ((PetscScalar) neltot - nNonDesign0 - nNonDesign1 - nNonDesign2)
          - volfrac;
  VecSet (dgdx,
      1.0 / ((PetscScalar) neltot - nNonDesign0 - nNonDesign1 - nNonDesign2));
  VecPointwiseMult (dgdx, dgdx, tmpVec0);
  VecPointwiseMult (dgdx, dgdx, tmpVec1);
  VecPointwiseMult (dgdx, dgdx, tmpVec2);

  VecRestoreArray (xPhys, &xp);
  VecGetArray (xPassive0, &xPassive0p);
  VecGetArray (xPassive1, &xPassive1p);
  VecGetArray (xPassive2, &xPassive2p);
  VecRestoreArray (Uloc, &up);
  VecRestoreArray (dfdx, &df);
  VecDestroy (&Uloc);

  VecDestroy (&tmpVec0);
  VecDestroy (&tmpVec1);
  VecDestroy (&tmpVec2);
  VecDestroy (&tmpxPhys);

  return (ierr);
}

PetscErrorCode LinearHeatConduction::WriteRestartFiles () {

  PetscErrorCode ierr = 0;

  // Only dump data if correct allocater has been used
  if (!restart) {
    return -1;
  }

  // Choose previous set of restart files
  if (flip) {
    flip = PETSC_FALSE;
  } else {
    flip = PETSC_TRUE;
  }

  // Open viewers for writing
  PetscViewer view; // vectors
  if (!flip) {
    PetscViewerBinaryOpen (PETSC_COMM_WORLD, filename00.c_str (),
        FILE_MODE_WRITE, &view);
  } else if (flip) {
    PetscViewerBinaryOpen (PETSC_COMM_WORLD, filename01.c_str (),
        FILE_MODE_WRITE, &view);
  }

  // Write vectors
  VecView (U, view);

  // Clean up
  PetscViewerDestroy (&view);

  return ierr;
}

//##################################################################
//##################################################################
//##################################################################
// ######################## PRIVATE ################################
//##################################################################
//##################################################################

PetscErrorCode LinearHeatConduction::AssembleConductivityMatrix (Vec xPhys,
    PetscScalar Emin, PetscScalar Emax, PetscScalar penal) {

  PetscErrorCode ierr;

  // Get the FE mesh structure (from the nodal mesh)
  PetscInt nel, nen;
  const PetscInt *necon;
#if DIM == 2
  ierr = DMDAGetElements_2D (da_nodal, &nel, &nen, &necon);
  CHKERRQ(ierr);
#elif DIM == 3
  ierr = DMDAGetElements_3D (da_nodal, &nel, &nen, &necon);
  CHKERRQ(ierr);
#endif

  // Get pointer to the densities
  PetscScalar *xp;
  VecGetArray (xPhys, &xp);

  // Zero the matrix
  MatZeroEntries (K);

  // Edof array, new
  PetscInt edof[nedof];
  PetscScalar ke[nedof * nedof];

  // Loop over elements, new
  for (PetscInt i = 0; i < nel; i++) {
    // loop over element nodes
    for (PetscInt j = 0; j < nen; j++) {
      // Get local dofs
      edof[j] = necon[i * nen + j];
    }
    // Use SIMP for heat conductivity interpolation
    PetscScalar dens = Emin + PetscPowScalar(xp[i], penal) * (Emax - Emin);
    for (PetscInt k = 0; k < nedof * nedof; k++) {
      ke[k] = KE[k] * dens;
    }
    // Add values to the sparse matrix
    ierr = MatSetValuesLocal (K, nedof, edof, nedof, edof, ke, ADD_VALUES);
    CHKERRQ(ierr);
  }
  MatAssemblyBegin (K, MAT_FINAL_ASSEMBLY);
  MatAssemblyEnd (K, MAT_FINAL_ASSEMBLY);

  // Impose the dirichlet conditions, i.e. K = N'*K*N - (N-I)
  // 1.: K = N'*K*N
  MatDiagonalScale (K, N, N);
  // 2. Add ones, i.e. K = K + NI, NI = I - N
  Vec NI;
  VecDuplicate (N, &NI);
  VecSet (NI, 1.0);
  VecAXPY (NI, -1.0, N);
  MatDiagonalSet (K, NI, ADD_VALUES);

  // Zero out possible loads in the RHS that coincide
  // with Dirichlet conditions
  VecPointwiseMult (RHS, RHS, N);

  VecDestroy (&NI);
  VecRestoreArray (xPhys, &xp);
  DMDARestoreElements (da_nodal, &nel, &nen, &necon);

  return ierr;
}

PetscErrorCode LinearHeatConduction::SetUpSolver () {

  PetscErrorCode ierr;

  // CHECK FOR RESTART POINT
  restart = PETSC_TRUE;
  flip = PETSC_TRUE;
  PetscBool flg, onlyDesign;
  onlyDesign = PETSC_FALSE;
  char filenameChar[PETSC_MAX_PATH_LEN];
  PetscOptionsGetBool (NULL, NULL, "-restart", &restart, &flg);
  PetscOptionsGetBool (NULL, NULL, "-onlyLoadDesign", &onlyDesign, &flg); // DONT READ DESIGN IF THIS IS TRUE

  // READ THE RESTART FILE INTO THE SOLUTION VECTOR(S)
  if (restart) {
    // THE FILES FOR WRITING RESTARTS
    std::string filenameWorkdir = "./";
    PetscOptionsGetString (
    NULL,
    NULL, "-workdir", filenameChar, sizeof(filenameChar), &flg);
    if (flg) {
      filenameWorkdir = "";
      filenameWorkdir.append (filenameChar);
    }
    filename00 = filenameWorkdir;
    filename01 = filenameWorkdir;
    filename00.append ("/RestartSol00.dat");
    filename01.append ("/RestartSol01.dat");

    // CHECK FOR SOLUTION AND READ TO STATE VECTOR(s)
    if (!onlyDesign) {
      // Where to read the restart point from
      std::string restartFileVec = ""; // NO RESTART FILE !!!!!
      // GET FILENAME
      PetscOptionsGetString (
      NULL,
      NULL, "-restartFileVecSol", filenameChar, sizeof(filenameChar), &flg);
      if (flg) {
        restartFileVec.append (filenameChar);
      }

      // PRINT TO SCREEN
      PetscPrintf (PETSC_COMM_WORLD,
          "# Restarting with solution (State Vector) from "
              "(-restartFileVecSol): %s \n", restartFileVec.c_str ());

      // Check if files exist:
      PetscBool vecFile = fexists (restartFileVec);
      if (!vecFile) {
        PetscPrintf (PETSC_COMM_WORLD, "File: %s NOT FOUND \n",
            restartFileVec.c_str ());
      }

      // READ
      if (vecFile) {
        PetscViewer view;
        // Open the data files
        ierr = PetscViewerBinaryOpen (PETSC_COMM_WORLD, restartFileVec.c_str (),
            FILE_MODE_READ, &view);

        VecLoad (U, view);

        PetscViewerDestroy (&view);
      }
    }
  }

  PC pc;

  // The fine grid Krylov method
  KSPCreate (PETSC_COMM_WORLD, &(ksp));

  // SET THE DEFAULT SOLVER PARAMETERS
  // The fine grid solver settings
  PetscScalar rtol = 1.0e-5; //new tmp
  //  PetscScalar rtol         = 1.0e-5;
  PetscScalar atol = 1.0e-50;
  PetscScalar dtol = 1.0e5;
  PetscInt restart = 100;
  PetscInt maxitsGlobal = 200;

  // Coarsegrid solver
  PetscScalar coarse_rtol = 1.0e-8;
  PetscScalar coarse_atol = 1.0e-50;
  PetscScalar coarse_dtol = 1e5;
  PetscInt coarse_maxits = 30;
  PetscInt coarse_restart = 30;

  // Number of smoothening iterations per up/down smooth_sweeps
  PetscInt smooth_sweeps = 4;

  // Set up the solver
  ierr = KSPSetType (ksp, KSPFGMRES); // KSPCG, KSPGMRES
  CHKERRQ(ierr);

  ierr = KSPGMRESSetRestart (ksp, restart);
  CHKERRQ(ierr);

  ierr = KSPSetTolerances (ksp, rtol, atol, dtol, maxitsGlobal);
  CHKERRQ(ierr);

  ierr = KSPSetInitialGuessNonzero (ksp, PETSC_TRUE);
  CHKERRQ(ierr);

  ierr = KSPSetOperators (ksp, K, K);
  CHKERRQ(ierr);

  // The preconditinoer
  KSPGetPC (ksp, &pc);
  // Make PCMG the default solver
  PCSetType (pc, PCMG);

  // Set solver from options
  KSPSetFromOptions (ksp);

  // Get the prec again - check if it has changed
  KSPGetPC (ksp, &pc);

  // Flag for pcmg pc
  PetscBool pcmg_flag = PETSC_TRUE;
  PetscObjectTypeCompare ((PetscObject) pc, PCMG, &pcmg_flag);

  // Only if PCMG is used
  if (pcmg_flag) {

    // DMs for grid hierachy
    DM *da_list, *daclist;
    Mat R;

    PetscMalloc(sizeof(DM) * nlvls, &da_list);
    for (PetscInt k = 0; k < nlvls; k++)
      da_list[k] = NULL;
    PetscMalloc(sizeof(DM) * nlvls, &daclist);
    for (PetscInt k = 0; k < nlvls; k++)
      daclist[k] = NULL;

    // Set 0 to the finest level
    daclist[0] = da_nodal;

    // Coordinates, new
#if DIM == 2
    PetscReal xmin = xc[0], xmax = xc[1], ymin = xc[2], ymax = xc[3];
#elif DIM == 3
    PetscReal xmin = xc[0], xmax = xc[1], ymin = xc[2], ymax = xc[3],
        zmin = xc[4], zmax = xc[5];
#endif
    // Set up the coarse meshes
    DMCoarsenHierarchy (da_nodal, nlvls - 1, &daclist[1]);
    for (PetscInt k = 0; k < nlvls; k++) {
      // NOTE: finest grid is nlevels - 1: PCMG MUST USE THIS ORDER ???
      da_list[k] = daclist[nlvls - 1 - k];
      // THIS SHOULD NOT BE NECESSARY
#if DIM == 2
      DMDASetUniformCoordinates (da_list[k], xmin, xmax, ymin, ymax, 0.0, 0.0);
#elif DIM == 3
      DMDASetUniformCoordinates (da_list[k], xmin, xmax, ymin, ymax, zmin,
          zmax);
#endif
    }
    // the PCMG specific options
    PCMGSetLevels (pc, nlvls, NULL);
    PCMGSetType (pc, PC_MG_MULTIPLICATIVE); // Default
    ierr = PCMGSetCycleType (pc, PC_MG_CYCLE_V);
    CHKERRQ(ierr);
    PCMGSetGalerkin (pc, PC_MG_GALERKIN_BOTH);
    for (PetscInt k = 1; k < nlvls; k++) {
      DMCreateInterpolation (da_list[k - 1], da_list[k], &R, NULL);
      PCMGSetInterpolation (pc, k, R);
      MatDestroy (&R);
    }

    // tidy up
    for (PetscInt k = 1; k < nlvls; k++) { // DO NOT DESTROY LEVEL 0
      DMDestroy (&daclist[k]);
    }
    PetscFree(da_list);
    PetscFree(daclist);

    // AVOID THE DEFAULT FOR THE MG PART
    {
      // SET the coarse grid solver:
      // i.e. get a pointer to the ksp and change its settings
      KSP cksp;
      PCMGGetCoarseSolve (pc, &cksp);
      // The solver
      ierr = KSPSetType (cksp, KSPGMRES); // KSPCG, KSPFGMRES
      ierr = KSPGMRESSetRestart (cksp, coarse_restart);
      // ierr = KSPSetType(cksp,KSPCG);

      ierr = KSPSetTolerances (cksp, coarse_rtol, coarse_atol, coarse_dtol,
          coarse_maxits);
      // The preconditioner
      PC cpc;
      KSPGetPC (cksp, &cpc);
      PCSetType (cpc, PCSOR); // PCGAMG, PCSOR, PCSPAI (NEEDS TO BE COMPILED), PCJACOBI

      // Set smoothers on all levels (except for coarse grid):
      for (PetscInt k = 1; k < nlvls; k++) {
        KSP dksp;
        PCMGGetSmoother (pc, k, &dksp);
        PC dpc;
        KSPGetPC (dksp, &dpc);
        ierr = KSPSetType (dksp,
        KSPGMRES); // KSPCG, KSPGMRES, KSPCHEBYSHEV (VERY GOOD FOR SPD)
        ierr = KSPGMRESSetRestart (dksp, smooth_sweeps);
        // ierr = KSPSetType(dksp,KSPCHEBYSHEV);
        ierr = KSPSetTolerances (dksp, PETSC_DEFAULT, PETSC_DEFAULT,
        PETSC_DEFAULT, smooth_sweeps); // NOTE in the above maxitr=restart;
        PCSetType (dpc, PCSOR); // PCJACOBI, PCSOR for KSPCHEBYSHEV very good
      }
    }
  }

  // Write check to screen:
  // Check the overall Krylov solver
  KSPType ksptype;
  KSPGetType (ksp, &ksptype);
  PCType pctype;
  PCGetType (pc, &pctype);
  PetscInt mmax;
  KSPGetTolerances (ksp, NULL, NULL, NULL, &mmax);
  PetscPrintf (PETSC_COMM_WORLD,
      "##############################################################\n");
  PetscPrintf (PETSC_COMM_WORLD,
      "################# Linear solver settings #####################\n");
  PetscPrintf (PETSC_COMM_WORLD,
      "# Main solver: %s, prec.: %s, maxiter.: %i \n", ksptype, pctype, mmax);

  // Only if pcmg is used
  if (pcmg_flag) {
    // Check the smoothers and coarse grid solver:
    for (PetscInt k = 0; k < nlvls; k++) {
      KSP dksp;
      PC dpc;
      KSPType dksptype;
      PCMGGetSmoother (pc, k, &dksp);
      KSPGetType (dksp, &dksptype);
      KSPGetPC (dksp, &dpc);
      PCType dpctype;
      PCGetType (dpc, &dpctype);
      PetscInt mmax;
      KSPGetTolerances (dksp, NULL, NULL, NULL, &mmax);
      PetscPrintf (PETSC_COMM_WORLD,
          "# Level %i smoother: %s, prec.: %s, sweep: %i \n", k, dksptype,
          dpctype, mmax);
    }
  }
  PetscPrintf (PETSC_COMM_WORLD,
      "##############################################################\n");

  return (ierr);
}

#if DIM == 2
PetscErrorCode LinearHeatConduction::DMDAGetElements_2D (DM dm, PetscInt *nel,
    PetscInt *nen, const PetscInt *e[]) {
  PetscErrorCode ierr;
  DM_DA *da = (DM_DA*) dm->data;
  PetscInt i, xs, xe, Xs, Xe;
  PetscInt j, ys, ye, Ys, Ye;
  PetscInt cnt = 0, cell[4], ns = 1, nn = 4;
  PetscInt c;
  if (!da->e) {
    if (da->elementtype == DMDA_ELEMENT_Q1) {
      ns = 1;
      nn = 4;
    }
    ierr = DMDAGetCorners (dm, &xs, &ys, NULL, &xe, &ye, NULL);
    CHKERRQ(ierr);
    ierr = DMDAGetGhostCorners (dm, &Xs, &Ys, NULL, &Xe, &Ye, NULL);
    CHKERRQ(ierr);
    xe += xs;
    Xe += Xs;
    if (xs != Xs) xs -= 1;
    ye += ys;
    Ye += Ys;
    if (ys != Ys) ys -= 1;

    da->ne = ns * (xe - xs - 1) * (ye - ys - 1);
    PetscMalloc((1 + nn * da->ne) * sizeof(PetscInt), &da->e);
    for (j = ys; j < ye - 1; j++) {
      for (i = xs; i < xe - 1; i++) {
        cell[0] = (i - Xs) + (j - Ys) * (Xe - Xs);
        cell[1] = (i - Xs + 1) + (j - Ys) * (Xe - Xs);
        cell[2] = (i - Xs + 1) + (j - Ys + 1) * (Xe - Xs);
        cell[3] = (i - Xs) + (j - Ys + 1) * (Xe - Xs);
        if (da->elementtype == DMDA_ELEMENT_Q1) {
          for (c = 0; c < ns * nn; c++)
            da->e[cnt++] = cell[c];
        }
      }
    }
  }
  *nel = da->ne;
  *nen = nn;
  *e = da->e;
  return (0);
}

PetscInt LinearHeatConduction::Quad4Isoparametric (PetscScalar *X,
    PetscScalar *Y, PetscInt redInt, PetscScalar *ke) {
  // QUA4_ISOPARAMETRIC - Computes QUA4 isoparametric element matrices
  // The element heat conductivity matrix is computed as:
  //
  //       ke = int(int(B^T*k*B,x),y)
  //
  // For an isoparameteric element this integral becomes:
  //
  //       ke = int(int(B^T*k*B*det(J),xi=-1..1),eta=-1..1)
  //
  // where B is the more complicated expression:
  // B = [dx dy]*N
  // where
  // dx = [invJ11 invJ12]*[dxi deta]
  // dy = [invJ21 invJ22]*[dxi deta]
  //
  // Remark: The thermal conductivity is left out in the below
  // computations, because we multiply with it afterwards (the aim is
  // topology optimization).
  // Furthermore, this is not the most efficient code, but it is readable.
  //
  /////////////////////////////////////////////////////////////////////////////////
  //////// INPUT:
  // X, Y = Vectors containing the coordinates of the four nodes
  //               (x1,y1,x2,y2,x3,y3,x4,y4). Where node 1 is in the
  //               lower left corner, and node 2 is the next node
  //               counterclockwise.
  // redInt   = Reduced integration option boolean (here an integer).
  //                  redInt == 0 (false): Full integration
  //                  redInt == 1 (true): Reduced integration
  //
  //////// OUTPUT:
  // ke  = Element heat conductivity matrix. Needs to be multiplied with elasticity
  // modulus
  //
  //   Written 2013 at
  //   Department of Mechanical Engineering
  //   Technical University of Denmark (DTU).
  //
  //   Modified 2020 by Zhidong Brian Zhang at
  //   Multi-scale additive manufacturing lab (MSAM)
  //   University of Waterloo
  /////////////////////////////////////////////////////////////////////////////////

  //// COMPUTE ELEMENT heat conductivity MATRIX
  // Thermal conductivity
  PetscScalar kcond[2][2] = { { 1.0, 0.0 }, { 0.0, 1.0 } }; // assigned with an unit value
  // Gauss points (GP) and weigths
  // Two Gauss points in all directions (total of eight)
  PetscScalar GP[2] = { -0.577350269189626, 0.577350269189626 };
  // Corresponding weights
  PetscScalar W[2] = { 1.0, 1.0 };
  // If reduced integration only use one GP
  if (redInt) {
    GP[0] = 0.0;
    W[0] = 2.0;
  }
  PetscScalar dNdxi[4];
  PetscScalar dNdeta[4];
  PetscScalar J[2][2];
  PetscScalar invJ[2][2];
  PetscScalar beta[2];
  PetscScalar B[2][4]; // Note: Small enough to be allocated on stack
  PetscScalar *dN;
  // Make sure the heat conductivity matrix is zeroed out:
  memset (ke, 0, sizeof(ke[0]) * 4 * 4);
  // Perform the numerical integration
  for (PetscInt ii = 0; ii < 2 - redInt; ii++) {
    for (PetscInt jj = 0; jj < 2 - redInt; jj++) {
      // Integration point
      PetscScalar xi = GP[ii];
      PetscScalar eta = GP[jj];
      // Differentiated shape functions
      DifferentiatedShapeFunctions_2D (xi, eta, dNdxi, dNdeta);
      // Jacobian
      J[0][0] = Dot (dNdxi, X, 4);
      J[0][1] = Dot (dNdxi, Y, 4);
      J[1][0] = Dot (dNdeta, X, 4);
      J[1][1] = Dot (dNdeta, Y, 4);
      // Inverse and determinant
      PetscScalar detJ = Inverse2M (J, invJ);
      // Weight factor at this point
      PetscScalar weight = W[ii] * W[jj] * detJ;
      // Strain-displacement matrix
      memset (B, 0, sizeof(B[0][0]) * 2 * 4); // zero out
      for (PetscInt ll = 0; ll < DIM; ll++) {
        // Add contributions from the different derivatives
        if (ll == 0) {
          dN = dNdxi;
        }
        if (ll == 1) {
          dN = dNdeta;
        }

        // Assemble strain operator
        for (PetscInt i = 0; i < 2; i++) {
          beta[i] = invJ[i][ll];
        }
        // Add contributions to strain-displacement matrix
        for (PetscInt i = 0; i < 2; i++) {
          for (PetscInt j = 0; j < 4; j++) {
            B[i][j] = B[i][j] + beta[i] * dN[j];
          }
        }
      }
      // Finally, add to the element matrix
      for (PetscInt i = 0; i < 4; i++) {
        for (PetscInt j = 0; j < 4; j++) {
          for (PetscInt k = 0; k < 2; k++) {
            for (PetscInt l = 0; l < 2; l++) {
              ke[j + 4 * i] = ke[j + 4 * i] + weight * (B[k][i] * kcond[k][l] * B[l][j]);
            }
          }
        }
      }
    }
  }
  return 0;
}

void LinearHeatConduction::DifferentiatedShapeFunctions_2D (PetscScalar xi,
    PetscScalar eta, PetscScalar *dNdxi, PetscScalar *dNdeta) {
  // differentiatedShapeFunctions - Computes differentiated shape functions
  // At the point given by (xi, eta, zeta).
  // With respect to xi:
  dNdxi[0] = -0.25 * (1.0 - eta);
  dNdxi[1] = 0.25 * (1.0 - eta);
  dNdxi[2] = 0.25 * (1.0 + eta);
  dNdxi[3] = -0.25 * (1.0 + eta);
  // With respect to eta:
  dNdeta[0] = -0.25 * (1.0 - xi);
  dNdeta[1] = -0.25 * (1.0 + xi);
  dNdeta[2] = 0.25 * (1.0 + xi);
  dNdeta[3] = 0.25 * (1.0 - xi);
}

PetscScalar LinearHeatConduction::Inverse2M (PetscScalar J[][2],
    PetscScalar invJ[][2]) {
  // inverse3M - Computes the inverse of a 3x3 matrix
  PetscScalar detJ = J[0][0] * J[1][1] - J[0][1] * J[1][0];
  invJ[0][0] = J[1][1] / detJ;
  invJ[0][1] = -J[0][1] / detJ;
  invJ[1][0] = -J[1][0] / detJ;
  invJ[1][1] = J[0][0] / detJ;
  return detJ;
}

#elif DIM == 3
PetscErrorCode LinearHeatConduction::DMDAGetElements_3D (DM dm, PetscInt *nel,
    PetscInt *nen, const PetscInt *e[]) {
  PetscErrorCode ierr;
  DM_DA *da = (DM_DA*) dm->data;
  PetscInt i, xs, xe, Xs, Xe;
  PetscInt j, ys, ye, Ys, Ye;
  PetscInt k, zs, ze, Zs, Ze;
  PetscInt cnt = 0, cell[8], ns = 1, nn = 8;
  PetscInt c;
  if (!da->e) {
    if (da->elementtype == DMDA_ELEMENT_Q1) {
      ns = 1;
      nn = 8;
    }
    ierr = DMDAGetCorners (dm, &xs, &ys, &zs, &xe, &ye, &ze);
    CHKERRQ(ierr);
    ierr = DMDAGetGhostCorners (dm, &Xs, &Ys, &Zs, &Xe, &Ye, &Ze);
    CHKERRQ(ierr);
    xe += xs;
    Xe += Xs;
    if (xs != Xs) xs -= 1;
    ye += ys;
    Ye += Ys;
    if (ys != Ys) ys -= 1;
    ze += zs;
    Ze += Zs;
    if (zs != Zs) zs -= 1;
    da->ne = ns * (xe - xs - 1) * (ye - ys - 1) * (ze - zs - 1);
    PetscMalloc((1 + nn * da->ne) * sizeof(PetscInt), &da->e);
    for (k = zs; k < ze - 1; k++) {
      for (j = ys; j < ye - 1; j++) {
        for (i = xs; i < xe - 1; i++) {
          cell[0] = (i - Xs) + (j - Ys) * (Xe - Xs)
                    + (k - Zs) * (Xe - Xs) * (Ye - Ys);
          cell[1] = (i - Xs + 1) + (j - Ys) * (Xe - Xs)
                    + (k - Zs) * (Xe - Xs) * (Ye - Ys);
          cell[2] = (i - Xs + 1) + (j - Ys + 1) * (Xe - Xs)
                    + (k - Zs) * (Xe - Xs) * (Ye - Ys);
          cell[3] = (i - Xs) + (j - Ys + 1) * (Xe - Xs)
                    + (k - Zs) * (Xe - Xs) * (Ye - Ys);
          cell[4] = (i - Xs) + (j - Ys) * (Xe - Xs)
                    + (k - Zs + 1) * (Xe - Xs) * (Ye - Ys);
          cell[5] = (i - Xs + 1) + (j - Ys) * (Xe - Xs)
                    + (k - Zs + 1) * (Xe - Xs) * (Ye - Ys);
          cell[6] = (i - Xs + 1) + (j - Ys + 1) * (Xe - Xs)
                    + (k - Zs + 1) * (Xe - Xs) * (Ye - Ys);
          cell[7] = (i - Xs) + (j - Ys + 1) * (Xe - Xs)
                    + (k - Zs + 1) * (Xe - Xs) * (Ye - Ys);
          if (da->elementtype == DMDA_ELEMENT_Q1) {
            for (c = 0; c < ns * nn; c++)
              da->e[cnt++] = cell[c];
          }
        }
      }
    }
  }
  *nel = da->ne;
  *nen = nn;
  *e = da->e;
  return (0);
}

PetscInt LinearHeatConduction::Hex8Isoparametric (PetscScalar *X,
    PetscScalar *Y, PetscScalar *Z, PetscInt redInt, PetscScalar *ke) {
  // HEX8_ISOPARAMETRIC - Computes HEX8 isoparametric element matrices
  // The element conduction matrix is computed as:
  //
  //       ke = int(int(int(B^T*k*B,x),y),z)
  //
  // For an isoparameteric element this integral becomes:
  //
  //       ke = int(int(int(B^T*k*B*det(J),xi=-1..1),eta=-1..1),zeta=-1..1)
  //
  // where B is the more complicated expression:
  // B = [dx dy dz]*N
  // where
  // dx = [invJ11 invJ12 invJ13]*[dxi deta dzeta]
  // dy = [invJ21 invJ22 invJ23]*[dxi deta dzeta]
  // dy = [invJ31 invJ32 invJ33]*[dxi deta dzeta]
  //
  // Remark: The thermal conductivity is left out in the below
  // computations, because we multiply with it afterwards (the aim is
  // topology optimization).
  // Furthermore, this is not the most efficient code, but it is readable.
  //
  /////////////////////////////////////////////////////////////////////////////////
  //////// INPUT:
  // X, Y, Z  = Vectors containing the coordinates of the eight nodes
  //               (x1,y1,z1,x2,y2,z2,...,x8,y8,z8). Where node 1 is in the
  //               lower left corner, and node 2 is the next node
  //               counterclockwise (looking in the negative z-dir). Finish the
  //               x-y-plane and then move in the positive z-dir.
  // redInt   = Reduced integration option boolean (here an integer).
  //           	redInt == 0 (false): Full integration
  //           	redInt == 1 (true): Reduced integration
  //
  //////// OUTPUT:
  // ke  = Element conduction matrix. Needs to be multiplied with thermal
  // conductivity
  //
  //   Written 2013 at
  //   Department of Mechanical Engineering
  //   Technical University of Denmark (DTU).
  //
  //   Modified by Zhidong Brian Zhang
  //   August 2020
  //   University of Waterloo
  /////////////////////////////////////////////////////////////////////////////////

  //// COMPUTE ELEMENT CONDUCTIVITY MATRIX
  // Thermal conductivity
  PetscScalar kcond[3][3] = { { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 } }; // assigned with an unit value
  // Gauss points (GP) and weigths
  // Two Gauss points in all directions (total of eight)
  PetscScalar GP[2] = { -0.577350269189626, 0.577350269189626 };
  // Corresponding weights
  PetscScalar W[2] = { 1.0, 1.0 };
  // If reduced integration only use one GP
  if (redInt) {
    GP[0] = 0.0;
    W[0] = 2.0;
  }
  // Matrices that help when we gather the strain-displacement matrix:
  PetscScalar dNdxi[8];
  PetscScalar dNdeta[8];
  PetscScalar dNdzeta[8];
  PetscScalar J[3][3];
  PetscScalar invJ[3][3];
  PetscScalar beta[3];
  PetscScalar B[3][8]; // Note: Small enough to be allocated on stack
  PetscScalar *dN;
  // Make sure the heat conductivity matrix is zeroed out:
  memset (ke, 0, sizeof(ke[0]) * 8 * 8);
  // Perform the numerical integration
  for (PetscInt ii = 0; ii < 2 - redInt; ii++) {
    for (PetscInt jj = 0; jj < 2 - redInt; jj++) {
      for (PetscInt kk = 0; kk < 2 - redInt; kk++) {
        // Integration point
        PetscScalar xi = GP[ii];
        PetscScalar eta = GP[jj];
        PetscScalar zeta = GP[kk];
        // Differentiated shape functions
        DifferentiatedShapeFunctions (xi, eta, zeta, dNdxi, dNdeta, dNdzeta);
        // Jacobian
        J[0][0] = Dot (dNdxi, X, 8);
        J[0][1] = Dot (dNdxi, Y, 8);
        J[0][2] = Dot (dNdxi, Z, 8);
        J[1][0] = Dot (dNdeta, X, 8);
        J[1][1] = Dot (dNdeta, Y, 8);
        J[1][2] = Dot (dNdeta, Z, 8);
        J[2][0] = Dot (dNdzeta, X, 8);
        J[2][1] = Dot (dNdzeta, Y, 8);
        J[2][2] = Dot (dNdzeta, Z, 8);
        // Inverse and determinant
        PetscScalar detJ = Inverse3M (J, invJ);
        // Weight factor at this point
        PetscScalar weight = W[ii] * W[jj] * W[kk] * detJ;
        // Strain-displacement matrix
        memset (B, 0, sizeof(B[0][0]) * 3 * 8); // zero out
        for (PetscInt ll = 0; ll < 3; ll++) {
          // Add contributions from the different derivatives
          if (ll == 0) {
            dN = dNdxi;
          }
          if (ll == 1) {
            dN = dNdeta;
          }
          if (ll == 2) {
            dN = dNdzeta;
          }
          // Assemble strain operator
          for (PetscInt i = 0; i < 3; i++) {
            beta[i] = invJ[i][ll];
          }

          // Add contributions to strain-displacement matrix
          for (PetscInt i = 0; i < 3; i++) {
            for (PetscInt j = 0; j < 8; j++) {
              B[i][j] = B[i][j] + beta[i] * dN[j];
            }
          }
        }
        // Finally, add to the element matrix
        for (PetscInt i = 0; i < 8; i++) {
          for (PetscInt j = 0; j < 8; j++) {
            for (PetscInt k = 0; k < 3; k++) {
              for (PetscInt l = 0; l < 3; l++) {
                ke[j + 8 * i] = ke[j + 8 * i]
                    + weight * (B[k][i] * kcond[k][l] * B[l][j]);
              }
            }
          }
        }
      }
    }
  }
  return 0;
}

void LinearHeatConduction::DifferentiatedShapeFunctions (PetscScalar xi,
    PetscScalar eta, PetscScalar zeta, PetscScalar *dNdxi, PetscScalar *dNdeta,
    PetscScalar *dNdzeta) {
// differentiatedShapeFunctions - Computes differentiated shape functions
// At the point given by (xi, eta, zeta).
// With respect to xi:
  dNdxi[0] = -0.125 * (1.0 - eta) * (1.0 - zeta);
  dNdxi[1] = 0.125 * (1.0 - eta) * (1.0 - zeta);
  dNdxi[2] = 0.125 * (1.0 + eta) * (1.0 - zeta);
  dNdxi[3] = -0.125 * (1.0 + eta) * (1.0 - zeta);
  dNdxi[4] = -0.125 * (1.0 - eta) * (1.0 + zeta);
  dNdxi[5] = 0.125 * (1.0 - eta) * (1.0 + zeta);
  dNdxi[6] = 0.125 * (1.0 + eta) * (1.0 + zeta);
  dNdxi[7] = -0.125 * (1.0 + eta) * (1.0 + zeta);
// With respect to eta:
  dNdeta[0] = -0.125 * (1.0 - xi) * (1.0 - zeta);
  dNdeta[1] = -0.125 * (1.0 + xi) * (1.0 - zeta);
  dNdeta[2] = 0.125 * (1.0 + xi) * (1.0 - zeta);
  dNdeta[3] = 0.125 * (1.0 - xi) * (1.0 - zeta);
  dNdeta[4] = -0.125 * (1.0 - xi) * (1.0 + zeta);
  dNdeta[5] = -0.125 * (1.0 + xi) * (1.0 + zeta);
  dNdeta[6] = 0.125 * (1.0 + xi) * (1.0 + zeta);
  dNdeta[7] = 0.125 * (1.0 - xi) * (1.0 + zeta);
// With respect to zeta:
  dNdzeta[0] = -0.125 * (1.0 - xi) * (1.0 - eta);
  dNdzeta[1] = -0.125 * (1.0 + xi) * (1.0 - eta);
  dNdzeta[2] = -0.125 * (1.0 + xi) * (1.0 + eta);
  dNdzeta[3] = -0.125 * (1.0 - xi) * (1.0 + eta);
  dNdzeta[4] = 0.125 * (1.0 - xi) * (1.0 - eta);
  dNdzeta[5] = 0.125 * (1.0 + xi) * (1.0 - eta);
  dNdzeta[6] = 0.125 * (1.0 + xi) * (1.0 + eta);
  dNdzeta[7] = 0.125 * (1.0 - xi) * (1.0 + eta);
}

PetscScalar LinearHeatConduction::Inverse3M (PetscScalar J[][3],
    PetscScalar invJ[][3]) {
// inverse3M - Computes the inverse of a 3x3 matrix
  PetscScalar detJ = J[0][0] * (J[1][1] * J[2][2] - J[2][1] * J[1][2])
      - J[0][1] * (J[1][0] * J[2][2] - J[2][0] * J[1][2])
                     + J[0][2] * (J[1][0] * J[2][1] - J[2][0] * J[1][1]);
  invJ[0][0] = (J[1][1] * J[2][2] - J[2][1] * J[1][2]) / detJ;
  invJ[0][1] = -(J[0][1] * J[2][2] - J[0][2] * J[2][1]) / detJ;
  invJ[0][2] = (J[0][1] * J[1][2] - J[0][2] * J[1][1]) / detJ;
  invJ[1][0] = -(J[1][0] * J[2][2] - J[1][2] * J[2][0]) / detJ;
  invJ[1][1] = (J[0][0] * J[2][2] - J[0][2] * J[2][0]) / detJ;
  invJ[1][2] = -(J[0][0] * J[1][2] - J[0][2] * J[1][0]) / detJ;
  invJ[2][0] = (J[1][0] * J[2][1] - J[1][1] * J[2][0]) / detJ;
  invJ[2][1] = -(J[0][0] * J[2][1] - J[0][1] * J[2][0]) / detJ;
  invJ[2][2] = (J[0][0] * J[1][1] - J[1][0] * J[0][1]) / detJ;
  return detJ;
}
#endif

PetscScalar LinearHeatConduction::Dot (PetscScalar *v1, PetscScalar *v2,
    PetscInt l) {
// Function that returns the dot product of v1 and v2,
// which must have the same length l
  PetscScalar result = 0.0;
  for (PetscInt i = 0; i < l; i++) {
    result = result + v1[i] * v2[i];
  }
  return result;
}
