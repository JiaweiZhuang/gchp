// $Id$
//
// Earth System Modeling Framework
// Copyright 2002-2016, University Corporation for Atmospheric Research, 
// Massachusetts Institute of Technology, Geophysical Fluid Dynamics 
// Laboratory, University of Michigan, National Centers for Environmental 
// Prediction, Los Alamos National Laboratory, Argonne National Laboratory, 
// NASA Goddard Space Flight Center.
// Licensed under the University of Illinois-NCSA License.
//
//==============================================================================

// Take out if MOAB isn't being used
#if defined ESMF_MOAB

#include <Mesh/include/ESMCI_Interp.h>

#include <Mesh/include/ESMCI_MBMesh.h>
#include <Mesh/include/ESMCI_MBMesh_Bilinear.h>
#include <Mesh/include/ESMCI_MBMesh_BBox.h>
#include <Mesh/include/ESMCI_MBMesh_Search_EToP.h>
#include <Mesh/include/ESMCI_MBMesh_Util.h>
#include <Mesh/include/ESMCI_MBMesh_Rendez_EtoP.h>

#include "ESMCI_PointList.h"

#include <iostream>
#include <iterator>
#include <iomanip>
#include <cmath>
#include <vector>
#include <map>

#include <ESMCI_VM.h>
#include <ESMCI_LogErr.h>

using std::vector;
using std::iterator;

//-----------------------------------------------------------------------------
// leave the following line as-is; it will insert the cvs ident string
// into the object file for tracking purposes.
static const char *const version = "$Id$";
//-----------------------------------------------------------------------------


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


using namespace ESMCI;


void pcoord_2d(vector<double> &p, vector<double> &a) {
    a[0] = (1.0 - p[0]) * (1.0 - p[1]);
    a[1] = p[0] * (1.0 - p[1]);
    a[2] = p[0] * p[1];
    a[3] = (1.0 - p[0]) * p[1];
}

void pcoord_3d(vector<double> &p, vector<double> &a) {
    a[0] = (1.0 - p[0]) * (1.0 - p[1]) * (1.0 - p[2]);
    a[1] = p[0] * (1.0 - p[1]) * (1.0 - p[2]);
    a[2] = p[0] * p[1] * (1.0 - p[2]);
    a[3] = (1.0 - p[0]) * p[1] * (1.0 - p[2]);
    a[4] = (1.0 - p[0]) * (1.0 - p[1]) * p[2];
    a[5] = p[0] * (1.0 - p[1]) * p[2];
    a[6] = p[0] * p[1] * p[2];
    a[7] = (1.0 - p[0]) * p[1] * p[2];
}

void calc_bilinear_mat(MBMesh *srcmb, PointList *dstpl,
  MBMesh_Search_EToP_Result_List &sres, IWeights &iw) {
  Trace __trace("calc_bilinear_mat(MBMesh &srcmb, PointList &dstpl, MBMesh_Search_EToP_Result_List &sres, IWeights &iw)");

  // Get MOAB mesh
  Interface *mesh = srcmb->mesh;

  // Find maximum number of dst nodes in search results
  unsigned int max_num_dst_nodes = 0;
  MBMesh_Search_EToP_Result_List::iterator sb = sres.begin(), se = sres.end();
  for (; sb != se; sb++) {
    // NOTE: sr.elem is a src element and sr.nodes is a list of dst nodes
    MBMesh_Search_EToP_Result &sr = **sb;

    // If there are no associated dst nodes then skip it
    if (sr.dst_nodes.size() > max_num_dst_nodes)
      max_num_dst_nodes=sr.dst_nodes.size();

   // iterate through nodes inside this search result element
    vector<etop_sr>::iterator db = sr.dst_nodes.begin(),
                                   de = sr.dst_nodes.end();
    for (; db != de; db++) {

      // Get all nodes on this element
      const EntityHandle* nodes;
      int num_nodes = 0;
      int merr= mesh-> get_connectivity(sr.src_elem, nodes, num_nodes);
      if (merr != MB_SUCCESS) {
        Throw() << "MOAB ERROR:: "<<moab::ErrorCodeStr[merr];
      }

      // get coords of dst_node
      vector<double> p;
      p.push_back(db->pcoord[0]);
      p.push_back(db->pcoord[1]);
      if (num_nodes > 4)
        p.push_back(db->pcoord[2]);

      // set up a vector to hold the weights
      vector<double> a;
      a.reserve(8);

      // weight generation
      if (num_nodes == 3) {
        a.push_back(db->pcoord[0]);
        a.push_back(db->pcoord[1]);
        a.push_back(1-db->pcoord[0]-db->pcoord[1]);
      }
      else if (num_nodes == 4)
        pcoord_2d(p, a);
      else if (num_nodes == 8)
        pcoord_3d(p, a);
      else
        Throw() << "invalid number of nodes";

      // build row of weight matrix
      int gid; MBMesh_get_gid(srcmb, sr.src_elem, &gid);
      IWeights::Entry row(db->dst_gid, 0, 0.0, gid);
      vector<IWeights::Entry> col;
      col.reserve(num_nodes);

      // Loop over nodes of the element
      for(int i = 0; i<num_nodes; ++i) {
        int gid; MBMesh_get_gid(srcmb, nodes[i], &gid);
        col.push_back(IWeights::Entry(gid, 0, a[i], db->dst_gid));
      }

      // insert the row
      iw.InsertRow(row, col);
    }
  }
}

void calc_bilinear_regrid_wgts(MBMesh *srcmb, PointList *dstpl, IWeights &wts) {
#define ESMC_METHOD "calc_bilinear_regrid_wgts()"

  // Get Parallel Information
  int localrc;
  int petCount = VM::getCurrent(&localrc)->getPetCount();
  if (ESMC_LogDefault.MsgFoundError(localrc,ESMCI_ERR_PASSTHRU,ESMC_CONTEXT,NULL))
    throw localrc;  // bail out with exception


  // Set meshes to use for regrid weight calculations
  MBMesh *srcmesh_regrid=srcmb;
  PointList *dstpl_regrid=dstpl;

  // If parallel then generate rendezvous meshes...and use them instead
  MBMesh *srcmesh_rend=NULL;
  PointList *dstpl_rend=NULL;
  if (petCount > 1) {

    // Create rendez meshes
    create_rendez_mbmesh_etop(srcmb, dstpl, &srcmesh_rend, &dstpl_rend);

    // Use rendezvous meshes instead
    srcmesh_regrid=srcmesh_rend;
    dstpl_regrid=dstpl_rend;
  }

  // Do search
  MBMesh_Search_EToP_Result_List result;
  MBMesh_Search_EToP(srcmesh_regrid, ESMCI_UNMAPPEDACTION_IGNORE,
                      dstpl_regrid, ESMCI_UNMAPPEDACTION_IGNORE,
                      1.0E-8, result);

  // Calculate the bilinear weight matrix
  calc_bilinear_mat(srcmesh_regrid, dstpl_regrid, result, wts);

  // If parallel then migrate weights back to decompostion of original mesh
  if (petCount > 1) {
    wts.Migrate(*dstpl);
  }

  // If parallel then get rid of rendezvous meshes.
  if (petCount > 1) {
    if (srcmesh_rend != NULL) delete srcmesh_rend;
    if (dstpl_rend != NULL) delete dstpl_rend;
  }
}

#endif // ESMF_MOAB
