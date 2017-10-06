// Copyright (C) 2007 Murtazo Nazarov
// Licensed under the GNU LGPL Version 2.1.
//
// Existing code for Dirichlet BC is used
//
// Modified by Niclas Jansson, 2008-2012.
// Modified by N. Cem Degirmenci, 2017.
//
// First added:  2007-05-01
// Last changed: 2017-08-04

#include "ufc2/DirichletBC_CG1v_CG1.h"

#include <dolfin/fem/Form.h>
#include <dolfin/fem/SubSystem.h>
#include <dolfin/fem/UFC.h>
#include <dolfin/la/Matrix.h>
#include <dolfin/la/PETScMatrix.h>
#include <dolfin/main/MPI.h>
#include <dolfin/mesh/BoundaryMesh.h>
#include <dolfin/mesh/MeshData.h>
#include <dolfin/mesh/Vertex.h>
#include <dolfin/mesh/SubDomain.h>
#include <dolfin/parameter/parameters.h>

#include <ufc.h>

#include <cmath>
#include <cstring>
#include <map>

#if (__sgi)
#define fmax(a,b) (a > b ? a : b) ;
#endif

namespace dolfin
{

//-----------------------------------------------------------------------------
DirichletBC_CG1v_CG1::DirichletBC_CG1v_CG1(Mesh& mesh, SubDomain& sub_domain, uint subsys, Function* val_fun) :
    BoundaryCondition(),
    mesh(mesh),
    sub_domains(NULL),
    sub_domain(0),
    sub_domains_local(false),
    user_sub_domain(&sub_domain),
    As(NULL),
    row_block(NULL),
    zero_block(NULL),
    a1_indices_array(NULL),
    boundary(NULL),
    cell_map(NULL),
    vertex_map(NULL),value_fun(val_fun)
{
  // Initialize sub domain markers
  init(sub_domain);

  sub_system = SubSystem(0);
  sub_sys_num = subsys;
}
//-----------------------------------------------------------------------------
DirichletBC_CG1v_CG1::~DirichletBC_CG1v_CG1()
{
  // Delete sub domain markers if created locally
  if (sub_domains_local)
    delete sub_domains;

  if (As)
    delete As;

  if (a1_indices_array)
    delete[] a1_indices_array;
  if (row_block)
    delete[] row_block;
  if (zero_block)
    delete[] zero_block;
  if (boundary)
    delete boundary;
}
//-----------------------------------------------------------------------------
void DirichletBC_CG1v_CG1::apply(GenericMatrix& A, GenericVector& b, const Form& form)
{
  apply(A, b, form.dofMaps()[1], form);
}
//-----------------------------------------------------------------------------
void DirichletBC_CG1v_CG1::apply(GenericMatrix& A, GenericVector& b, const DofMap& dof_map,
                   const ufc::form& ufc_form)
{
  dolfin::error("Not implemented:\n",
      "void apply(GenericMatrix& A, GenericVector& b,\n",
      "\tDofMap const& dof_map, const ufc::form& form)");
}
//-----------------------------------------------------------------------------
void DirichletBC_CG1v_CG1::apply(GenericMatrix& A, GenericVector& b, const GenericVector& x,
                   const Form& form)
{
  apply(A, b, form.dofMaps()[1], form);
}
//-----------------------------------------------------------------------------
void DirichletBC_CG1v_CG1::apply(GenericMatrix& A, GenericVector& b, const GenericVector& x,
                   const DofMap& dof_map, const ufc::form& ufc_form)
{
  dolfin::error("Not implemented:\n",
      "void apply(GenericMatrix& A, GenericVector& b, const GenericVector& x,\n",
      "\tDofMap const& dof_map, const ufc::form& form)");
}

//-----------------------------------------------------------------------------
void DirichletBC_CG1v_CG1::apply(GenericMatrix& A, GenericVector& b, const DofMap& dof_map,
                   const Form& form)
{

  dolfin_set("output destination", "silent");
  if (MPI::processNumber() == 0)
    dolfin_set("output destination", "terminal");
  message("Applying DirichletBC_CG1v_CG1 boundary conditions to linear system.");

  UFC ufc(form.form(), mesh, form.dofMaps());
  if (boundary == 0)
  {
    boundary = new BoundaryMesh(mesh);
    if (boundary->numCells())
    {
      cell_map = boundary->data().meshFunction("cell map");
      vertex_map = boundary->data().meshFunction("vertex map");
    }
  }
  if (As == 0)
  {

    // Create data structure for local assembly data
    const std::string la_backend = dolfin_get("linear algebra backend");
    if (la_backend == "JANPACK")
    {
      As = new Matrix(A.size(0), A.size(1));
      *(As->instance()) = A;
      //      (*(As->instance())).down_cast<JANPACKMat>().dup(A);
    }
    else
    {
      As = new Matrix();
      (*(As->instance())).down_cast<PETScMatrix>().dup(A);
    }

    if (MPI::numProcesses() > 1)
    {
      std::map<uint, uint> mapping;
      for (CellIterator c(mesh); !c.end(); ++c)
      {
        ufc.update(*c, mesh.distdata());
        (form.dofMaps())[0].tabulate_dofs(ufc.dofs[0], ufc.cell, c->index());

        for (uint j = 0; j < (form.dofMaps())[0].local_dimension(); j++)
          off_proc_rows.insert(ufc.dofs[0][j]);
      }

      b.init_ghosted(off_proc_rows.size(), off_proc_rows, mapping);
    }

    row_block = new real[A.size(0)];
    zero_block = new real[A.size(0)];
    a1_indices_array = new uint[A.size(0)];
  }
  // Copy global stiffness matrix into temporary one
  *(As->instance()) = A;

  Array<uint> nodes;
  uint gdim = mesh.geometry().dim();
  uint cdim = mesh.type().numVertices(mesh.topology().dim());

  uint count = 0;
  if (boundary->numCells())
  {
    for (VertexIterator v(*boundary); !v.end(); ++v)
    {
      Vertex vertex(mesh, vertex_map->get(*v));

      // Skip facets not inside the sub domain
      if ((*sub_domains)(vertex) != sub_domain)
      {
        continue;
      }

      uint node = vertex.index();
      if (!mesh.distdata().is_ghost(node, 0) || MPI::numProcesses() == 1)
      {
        Cell cell(mesh, (vertex.entities(gdim))[0]);

        uint *cvi = cell.entities(0);
        uint ci = 0;
        for (ci = 0; ci < cell.numEntities(0); ci++)
          if (cvi[ci] == node)
            break;

        ufc.update(cell, mesh.distdata());
        (form.dofMaps())[0].tabulate_dofs(ufc.dofs[0], ufc.cell, cell.index());

        // Get components of the vector-valued function at the current node.
	// 1 cg1 + gdim cg1v

       // for (uint i = 0; i < gdim; i++, ci += cdim)	
	
          nodes.push_back(ufc.dofs[0][ci + (cdim)*sub_sys_num]);

         real values[3];
	 
	 value_fun->eval(values, vertex.x());

        applyDirichletBC_CG1v_CG1((Matrix&) A, *As, (Vector&) b, mesh, node, nodes, values[0]);
        count++;
        nodes.clear();
      }
    }
  }

  // Apply changes in the temporary matrix
  As->apply();

  // Apply changes in the stiffness matrix and load vector
  A = *(As->instance());
  b.apply();

}
//-----------------------------------------------------------------------------
void DirichletBC_CG1v_CG1::init(SubDomain& sub_domain)
{
  // Create mesh function for sub domain markers on facets
  mesh.init(0);
  sub_domains = new MeshFunction<uint>(mesh, 0);
  sub_domains_local = true;

  // Mark everything as sub domain 1
  (*sub_domains) = 1;

  // Mark the sub domain as sub domain 0
  sub_domain.mark(*sub_domains, 0);
}

//-----------------------------------------------------------------------------
void DirichletBC_CG1v_CG1::applyDirichletBC_CG1v_CG1(Matrix& A, Matrix& As, Vector& b, Mesh& mesh,
                         uint node, Array<uint>& nodes, real impedance_val)
{

  int nsdim = mesh.topology().dim();

  // Get number of nozero elements in the rows of A
  //FIXME implement nzmax in dolfin 0.8
  int nzm = A.size(0);

  uint a1_ncols;
  Array<real> a1;
  Array<uint> a1_indices;
  a1_ncols = 0;

  A.getrow(nodes[0], a1_indices, a1);
  a1_ncols = a1_indices.size();

  nzm = a1_ncols;
  memset(row_block, 0.0, nzm * sizeof(real));
  memset(zero_block, 0.0, nzm * sizeof(real));

  std::copy(a1_indices.begin(), a1_indices.end(), a1_indices_array);

  As.set(zero_block, 1, &nodes[0], static_cast<uint>(a1_ncols),
       a1_indices_array);

   Aset(As, nodes[0], nodes[0], 1.0);

   bset(b, nodes[0], impedance_val);
}
}
//-----------------------------------------------------------------------------
