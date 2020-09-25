.. index:: compute rdf

compute rdf command
===================

Syntax
""""""

.. parsed-literal::

   compute ID group-ID born

* ID, group-ID are documented in :doc:`compute <compute>` command
* born = style name of this compute command

Examples
""""""""

.. code-block:: LAMMPS

   compute 1 all born

Description
"""""""""""

Define a computation that calculates the second derivatives of the potential
energy with regard to strain tensor elements. These values can be used to
compute:
:math:`C^{B}=\frac{1}{V}\frac{\partial{}U}{\partial{}\varepsilon_{i}\varepsilon_{j}`
also called the Born term. This quantity can be used to compute the elastic
constant tensor using the stress-stress fluctuations formalism. Using the
symmetric Voigt notation, the elastic constant tensor can be written as a 6x6
symmetric matrix:

.. math::

    C_{i,j} = \langleC^{B}_{i,j}\rangle
             + \langle\sigma_{i}\sigma_{j}\rangle
             - \langle\sigma_{i}\rangle\langle\sigma_{j}\rangle
             + \frac{Nk_{B}T}{V}
               \left(\delta_{i,j}+\delta_{1,j}+\delta_{2,j}+\delta_{3,j}\right)

where :math:`\sigma` stands for the virial stress tensor, :math:`\delta` is the
Kronecker delta and the usual notation apply for the number of particle, the
temperature and volume respectively :math:`N`, :math:`T` and :math:`V`.

The Born term is a symmetric 6x6 matrix by construction and as such can be
expressed as 21 independent terms. The terms are ordered corresponding to the
following matrix element:

.. math::

    \matrix{
       C_{1}  & C_{7}   & C_{8}  & C_{9}  & C_{10} & C_{11} \\
       C_{7}  & C_{2}   & C_{12} & C_{13} & C_{14} & C_{15} \\
       C_{8}  & C_{12}  & C_{3}  & C_{16} & C_{17} & C_{18} \\
       C_{9}  & C_{13}  & C_{16} & C_{4}  & C_{19} & C_{20} \\
       C_{10} & C_{14}  & C_{17} & C_{19} & C_{5}  & C_{21} \\
       C_{11} & C_{15}  & C_{18} & C_{20} & C_{12} & C_{6}  \\

in this matrix the indices are the corresponding term index in the compute
output. Each term comes from the sum of every interactions derivatives in the
system as explained in :ref:`(VanWorkum) <VanWorkum>` or
:ref:`(Voyiatzis) <Voyiatzis>`.

The output can be accessed using usual Lammps routines:
.. code-block:: LAMMPS

   compute 1 all born
   compute 2 all pressure NULL virial
   variable S1 equal -c_2[1]
   variable S2 equal -c_2[2]
   variable S3 equal -c_2[3]
   variable S4 equal -c_2[4]
   variable S5 equal -c_2[5]
   variable S6 equal -c_2[6]
   fix 1 all ave/time 1 1 100 v_S1 v_S2 v_S3 v_S4 v_S5 v_S6 c_1[*] file born.out

In this example, the file *born.out* will contain the information needed to compute
the first and second terms of the elastic constant matrix in a post processing procedure.
The other required quantities can be accessed using any other *LAMMPS* usual method.

**Output info:**

This compute calculates a global array with the number of rows=21.
The values are ordered as explained above. These values can be used
by any command that uses a global values from a compute as input. See
the :doc:`Howto output <Howto_output>` doc page for an overview of
LAMMPS output options.

The array values calculated by this compute are all "extensive".

Restrictions
""""""""""""

The Born term can be decomposed as a product of two terms. The first one
is a general term which depends on the configuration. The second one is
specific to every interaction composing your forcefield (non-bonded,
bonds, angle...). Currently not all interaction implement the born
method giving first and second order derivatives and an error will
be raised if you try to use this compute with such interactions.

Default
"""""""

none

.. _VanWorkum:
K. Van Workum et al. J. Chem. Phys. 125 144506 (2006)

.. _Voyiatzis:
E.Voyiatzis, Computer Physics Communications 184(2013)27-33
