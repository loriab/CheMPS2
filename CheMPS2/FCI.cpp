/*
   CheMPS2: a spin-adapted implementation of DMRG for ab initio quantum chemistry
   Copyright (C) 2013-2015 Sebastian Wouters

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <climits>
#include <assert.h>
#include <iostream>
#include <math.h>
#include <sys/stat.h>
#include <sys/time.h>

using std::cout;
using std::endl;

#include "FCI.h"
#include "Irreps.h"
#include "Lapack.h"
#include "Davidson.h"

CheMPS2::FCI::FCI(Hamiltonian * Ham, const unsigned int theNel_up, const unsigned int theNel_down, const int TargetIrrep_in, const double maxMemWorkMB_in, const int FCIverbose_in){

   // Copy the basic information
   FCIverbose   = FCIverbose_in;
   maxMemWorkMB = maxMemWorkMB_in;
   L = Ham->getL();
   assert( theNel_up    <= L );
   assert( theNel_down  <= L );
   assert( maxMemWorkMB >  0.0 );
   Nel_up   = theNel_up;
   Nel_down = theNel_down;
   
   // Construct the irrep product table and the list with the orbitals irreps
   CheMPS2::Irreps myIrreps( Ham->getNGroup() );
   NumIrreps         = myIrreps.getNumberOfIrreps();
   TargetIrrep       = TargetIrrep_in;
   orb2irrep         = new int[ L ];
   for (unsigned int orb = 0; orb < L; orb++){ orb2irrep[ orb ] = Ham->getOrbitalIrrep( orb ); }

   /* Copy the Hamiltonian over:
         G_ij = T_ij - 0.5 \sum_k <ik|kj> and ERI_{ijkl} = <ij|kl>
         <ij|kl> is the electron repulsion integral, int dr1 dr2 i(r1) j(r1) k(r2) l(r2) / |r1-r2| */
   Econstant = Ham->getEconst();
   Gmat = new double[ L * L ];
   ERI  = new double[ L * L * L * L ];
   for (unsigned int orb1 = 0; orb1 < L; orb1++){
      for (unsigned int orb2 = 0; orb2 < L; orb2++){
         double tempvar = 0.0;
         for (unsigned int orb3 = 0; orb3 < L; orb3++){
            tempvar += Ham->getVmat( orb1, orb3, orb3, orb2 );
            for (unsigned int orb4 = 0; orb4 < L; orb4++){
               // CheMPS2::Hamiltonian uses physics notation ; ERI chemists notation.
               ERI[ orb1 + L * ( orb2 + L * ( orb3 + L * orb4 ) ) ] = Ham->getVmat( orb1 , orb3 , orb2 , orb4 );
            }
         }
         Gmat[ orb1 + L * orb2 ] = Ham->getTmat( orb1 , orb2 ) - 0.5 * tempvar;
      }
   }
   
   // Set all other internal variables
   StartupCountersVsBitstrings();
   StartupLookupTables();
   StartupIrrepCenter();

}

CheMPS2::FCI::~FCI(){
   
   // FCI::FCI
   delete [] orb2irrep;
   delete [] Gmat;
   delete [] ERI;
   
   // FCI::StartupCountersVsBitstrings
   for ( unsigned int irrep=0; irrep<NumIrreps; irrep++ ){
      delete [] str2cnt_up[irrep];
      delete [] str2cnt_down[irrep];
      delete [] cnt2str_up[irrep];
      delete [] cnt2str_down[irrep];
   }
   delete [] str2cnt_up;
   delete [] str2cnt_down;
   delete [] cnt2str_up;
   delete [] cnt2str_down;
   delete [] numPerIrrep_up;
   delete [] numPerIrrep_down;
   
   // FCI::StartupLookupTables
   for ( unsigned int irrep=0; irrep<NumIrreps; irrep++ ){
      delete [] lookup_cnt_alpha[irrep];
      delete [] lookup_cnt_beta[irrep];
      delete [] lookup_irrep_alpha[irrep];
      delete [] lookup_irrep_beta[irrep];
      delete [] lookup_sign_alpha[irrep];
      delete [] lookup_sign_beta[irrep];
   }
   delete [] lookup_cnt_alpha;
   delete [] lookup_cnt_beta;
   delete [] lookup_irrep_alpha;
   delete [] lookup_irrep_beta;
   delete [] lookup_sign_alpha;
   delete [] lookup_sign_beta;
   
   // FCI::StartupIrrepCenter
   for ( unsigned int irrep=0; irrep<NumIrreps; irrep++ ){
      delete [] irrep_center_crea_orb[irrep];
      delete [] irrep_center_anni_orb[irrep];
      delete [] irrep_center_jumps[irrep];
   }
   delete [] irrep_center_crea_orb;
   delete [] irrep_center_anni_orb;
   delete [] irrep_center_jumps;
   delete [] irrep_center_num;
   delete [] HXVworksmall;
   delete [] HXVworkbig1;
   delete [] HXVworkbig2;

}

void CheMPS2::FCI::StartupCountersVsBitstrings(){

   // Can you represent the alpha and beta Slater determinants as unsigned integers?
   assert( L <= CHAR_BIT * sizeof(unsigned int) );
   
   // Variable which is only needed here: 2^L
   unsigned int TwoPowL = 1;
   for (unsigned int orb = 0; orb < L; orb++){ TwoPowL *= 2; }

   // Create the required arrays to perform the conversions between counters and bitstrings
   numPerIrrep_up     = new unsigned int[ NumIrreps ];
   numPerIrrep_down   = new unsigned int[ NumIrreps ];
   str2cnt_up         = new int*[ NumIrreps ];
   str2cnt_down       = new int*[ NumIrreps ];
   cnt2str_up         = new unsigned int*[ NumIrreps ];
   cnt2str_down       = new unsigned int*[ NumIrreps ];
   
   for (unsigned int irrep = 0; irrep < NumIrreps; irrep++){
      numPerIrrep_up  [ irrep ] = 0;
      numPerIrrep_down[ irrep ] = 0;
      str2cnt_up  [ irrep ] = new int[ TwoPowL ];
      str2cnt_down[ irrep ] = new int[ TwoPowL ];
   }
   
   int * bits = new int[ L ]; // Temporary helper array
   
   // Loop over all allowed bit strings in the spinless fermion Fock space
   for (unsigned int bitstring = 0; bitstring < TwoPowL; bitstring++){
   
      // Find the number of particles and the irrep which correspond to each basis vector
      str2bits( L , bitstring , bits );
      unsigned int Nparticles = 0;
      int Irrep = 0;
      for (unsigned int orb=0; orb<L; orb++){
         if ( bits[orb] ){
            Nparticles++;
            Irrep = getIrrepProduct( Irrep , getOrb2Irrep( orb ) );
         }
      }
      
      // If allowed: set the corresponding str2cnt to the correct counter and keep track of the number of allowed vectors
      for ( unsigned int irr = 0; irr < NumIrreps; irr++ ){
         str2cnt_up  [ irr ][ bitstring ] = -1;
         str2cnt_down[ irr ][ bitstring ] = -1;
      }
      if ( Nparticles == Nel_up ){
         str2cnt_up[ Irrep ][ bitstring ] = numPerIrrep_up[ Irrep ];
         numPerIrrep_up[ Irrep ]++;
      }
      if ( Nparticles == Nel_down ){
         str2cnt_down[ Irrep ][ bitstring ] = numPerIrrep_down[ Irrep ];
         numPerIrrep_down[ Irrep ]++;
      }
   
   }
   
   // Fill the reverse info array: cnt2str
   for ( unsigned int irrep = 0; irrep < NumIrreps; irrep++ ){
   
      if ( FCIverbose>1 ){
         cout << "FCI::Startup : For irrep " << irrep << " there are " << numPerIrrep_up  [ irrep ] << " alpha Slater determinants and "
                                                                       << numPerIrrep_down[ irrep ] <<  " beta Slater determinants." << endl;
      }
      
      cnt2str_up  [ irrep ] = new unsigned int[ numPerIrrep_up  [ irrep ] ];
      cnt2str_down[ irrep ] = new unsigned int[ numPerIrrep_down[ irrep ] ];
      for (unsigned int bitstring = 0; bitstring < TwoPowL; bitstring++){
         if ( str2cnt_up  [ irrep ][ bitstring ] != -1 ){ cnt2str_up  [ irrep ][ str2cnt_up  [ irrep ][ bitstring ] ] = bitstring; }
         if ( str2cnt_down[ irrep ][ bitstring ] != -1 ){ cnt2str_down[ irrep ][ str2cnt_down[ irrep ][ bitstring ] ] = bitstring; }
      }
   
   }
   
   delete [] bits; // Delete temporary helper array

}

void CheMPS2::FCI::StartupLookupTables(){

   // Create a bunch of stuff
   lookup_cnt_alpha   = new int*[ NumIrreps ];
   lookup_cnt_beta    = new int*[ NumIrreps ];
   lookup_irrep_alpha = new int*[ NumIrreps ];
   lookup_irrep_beta  = new int*[ NumIrreps ];
   lookup_sign_alpha  = new int*[ NumIrreps ];
   lookup_sign_beta   = new int*[ NumIrreps ];
   
   int * bits = new int[ L ]; // Temporary helper array
   
   // Quick lookup tables for " sign | new > = E^spinproj_{ij} | old >
   for ( unsigned int irrep = 0; irrep < NumIrreps; irrep++ ){
      
      lookup_cnt_alpha  [ irrep ] = new int[ L * L * numPerIrrep_up[ irrep ] ];
      lookup_irrep_alpha[ irrep ] = new int[ L * L * numPerIrrep_up[ irrep ] ];
      lookup_sign_alpha [ irrep ] = new int[ L * L * numPerIrrep_up[ irrep ] ];
      
      for ( unsigned int cnt_new_alpha = 0; cnt_new_alpha < numPerIrrep_up[ irrep ]; cnt_new_alpha++ ){
      
         for ( unsigned int i = 0; i < L; i++ ){
            for ( unsigned int j = 0; j < L; j++){
               // Check for the sign. If no check for sign, you multiply with sign 0 and everything should be OK...
               lookup_cnt_alpha  [irrep][ i + L * ( j + L * cnt_new_alpha ) ] = 0;
               lookup_irrep_alpha[irrep][ i + L * ( j + L * cnt_new_alpha ) ] = 0;
               lookup_sign_alpha [irrep][ i + L * ( j + L * cnt_new_alpha ) ] = 0;
            }
         }
      
         str2bits( L , cnt2str_up[ irrep ][ cnt_new_alpha ] , bits );
      
         int phase_creator = 1;
         for ( unsigned int creator = 0; creator < L; creator++ ){
            if ( bits[ creator ] ){
               bits[ creator ] = 0;
                  
               int phase_annihilator = 1;
               for ( unsigned int annihilator = 0; annihilator < L; annihilator++ ){
                  if ( !(bits[ annihilator ]) ){
                     bits[ annihilator ] = 1;
                     
                     const int irrep_old = getIrrepProduct( irrep , getIrrepProduct( getOrb2Irrep( creator ) , getOrb2Irrep( annihilator ) ) );
                     const int cnt_old = str2cnt_up[ irrep_old ][ bits2str( L , bits ) ];
                     const int phase = phase_creator * phase_annihilator;
                     
                     lookup_cnt_alpha  [irrep][ creator + L * ( annihilator + L * cnt_new_alpha ) ] = cnt_old;
                     lookup_irrep_alpha[irrep][ creator + L * ( annihilator + L * cnt_new_alpha ) ] = irrep_old;
                     lookup_sign_alpha [irrep][ creator + L * ( annihilator + L * cnt_new_alpha ) ] = phase;
                     
                     bits[ annihilator ] = 0;
                  } else {
                     phase_annihilator *= -1;
                  }
               }
               
               bits[ creator ] = 1;
               phase_creator *= -1;
            }
         }
      }
      
      lookup_cnt_beta  [ irrep ] = new int[ L * L * numPerIrrep_down[ irrep ] ];
      lookup_irrep_beta[ irrep ] = new int[ L * L * numPerIrrep_down[ irrep ] ];
      lookup_sign_beta [ irrep ] = new int[ L * L * numPerIrrep_down[ irrep ] ];
      
      for ( unsigned int cnt_new_beta = 0; cnt_new_beta < numPerIrrep_down[ irrep ]; cnt_new_beta++ ){
      
         for ( unsigned int i = 0; i < L; i++ ){
            for ( unsigned int j = 0; j < L; j++ ){
               // Check for the sign. If no check for sign, you multiply with sign and everything should be OK...
               lookup_cnt_beta  [irrep][ i + L * ( j + L * cnt_new_beta ) ] = 0;
               lookup_irrep_beta[irrep][ i + L * ( j + L * cnt_new_beta ) ] = 0;
               lookup_sign_beta [irrep][ i + L * ( j + L * cnt_new_beta ) ] = 0;
            }
         }
      
         str2bits( L , cnt2str_down[ irrep ][ cnt_new_beta ] , bits );
      
         int phase_creator = 1;
         for ( unsigned int creator = 0; creator < L; creator++ ){
            if ( bits[ creator ] ){
               bits[ creator ] = 0;
                  
               int phase_annihilator = 1;
               for ( unsigned int annihilator = 0; annihilator < L; annihilator++ ){
                  if ( !(bits[ annihilator ]) ){
                     bits[ annihilator ] = 1;
                     
                     const int irrep_old = getIrrepProduct( irrep , getIrrepProduct( getOrb2Irrep( creator ) , getOrb2Irrep( annihilator ) ) );
                     const int cnt_old = str2cnt_down[ irrep_old ][ bits2str( L , bits ) ];
                     const int phase = phase_creator * phase_annihilator;
                     
                     lookup_cnt_beta  [irrep][ creator + L * ( annihilator + L * cnt_new_beta ) ] = cnt_old;
                     lookup_irrep_beta[irrep][ creator + L * ( annihilator + L * cnt_new_beta ) ] = irrep_old;
                     lookup_sign_beta [irrep][ creator + L * ( annihilator + L * cnt_new_beta ) ] = phase;
                     
                     bits[ annihilator ] = 0;
                  } else {
                     phase_annihilator *= -1;
                  }
               }
               
               bits[ creator ] = 1;
               phase_creator *= -1;
            }
         }
      }
      
   }
   
   delete [] bits; // Delete temporary helper array

}

void CheMPS2::FCI::StartupIrrepCenter(){

   // Find the orbital combinations which can form a center irrep
   irrep_center_num      = new unsigned int [ NumIrreps ];
   irrep_center_crea_orb = new unsigned int*[ NumIrreps ];
   irrep_center_anni_orb = new unsigned int*[ NumIrreps ];
   
   for ( unsigned int irrep_center = 0; irrep_center < NumIrreps; irrep_center++ ){
      const int irrep_center_const_signed = irrep_center;
   
      irrep_center_num[ irrep_center ] = 0;
      for ( unsigned int creator = 0; creator < L; creator++ ){
         for ( unsigned int annihilator = creator; annihilator < L; annihilator++ ){
            if ( getIrrepProduct( getOrb2Irrep( creator ) , getOrb2Irrep( annihilator ) ) == irrep_center_const_signed ){
               irrep_center_num[ irrep_center ] += 1;
            }
         }
      }
      irrep_center_crea_orb[ irrep_center ] = new unsigned int[ irrep_center_num[ irrep_center ] ];
      irrep_center_anni_orb[ irrep_center ] = new unsigned int[ irrep_center_num[ irrep_center ] ];
      irrep_center_num[ irrep_center ] = 0;
      for ( unsigned int creator = 0; creator < L; creator++ ){
         for ( unsigned int annihilator = creator; annihilator < L; annihilator++){
            if ( getIrrepProduct( getOrb2Irrep( creator ) , getOrb2Irrep( annihilator ) ) == irrep_center_const_signed ){
               irrep_center_crea_orb[ irrep_center ][ irrep_center_num[ irrep_center ] ] = creator;
               irrep_center_anni_orb[ irrep_center ][ irrep_center_num[ irrep_center ] ] = annihilator;
               irrep_center_num[ irrep_center ] += 1;
            }
         }
      }
   
   }
   
   irrep_center_jumps = new unsigned long long*[ NumIrreps ];
   HXVsizeWorkspace = 0;
   for ( unsigned int irrep_center = 0; irrep_center < NumIrreps; irrep_center++ ){
   
      irrep_center_jumps[ irrep_center ] = new unsigned long long[ NumIrreps+1 ];
      const int localTargetIrrep = getIrrepProduct( irrep_center , getTargetIrrep() );
      irrep_center_jumps[ irrep_center ][ 0 ] = 0;
      for ( unsigned int irrep_up = 0; irrep_up < NumIrreps; irrep_up++ ){
         const int irrep_down = getIrrepProduct( irrep_up , localTargetIrrep );
         unsigned long long temp  = numPerIrrep_up  [ irrep_up   ];
                            temp *= numPerIrrep_down[ irrep_down ];
         irrep_center_jumps[ irrep_center ][ irrep_up+1 ] = irrep_center_jumps[ irrep_center ][ irrep_up ] + temp;
      }
      if ( irrep_center_num[ irrep_center ] * irrep_center_jumps[ irrep_center ][ NumIrreps ] > HXVsizeWorkspace ){
         HXVsizeWorkspace = irrep_center_num[ irrep_center ] * irrep_center_jumps[ irrep_center ][ NumIrreps ];
      }
   }
   if ( FCIverbose>0 ){
      cout << "FCI::Startup : Number of variables in the FCI vector = " << getVecLength(0) << endl;
      unsigned long long numberOfBytes = 2 * sizeof(double) * HXVsizeWorkspace;
      
      cout << "FCI::Startup : Without additional loops the FCI matrix-vector product requires a workspace of " << 1e-6 * numberOfBytes << " MB memory." << endl;
      if ( maxMemWorkMB < 1e-6 * numberOfBytes ){
         HXVsizeWorkspace = (unsigned long long) ceil( ( maxMemWorkMB * 1e6 ) / ( 2 * sizeof(double ) ) );
         numberOfBytes = 2 * sizeof(double) * HXVsizeWorkspace;
         cout << "               For practical purposes, the workspace is constrained to " << 1e-6 * numberOfBytes << " MB memory." << endl;
      }
   }
   HXVworksmall = new double[ L * L * L * L ];
   HXVworkbig1  = new double[ HXVsizeWorkspace ];
   HXVworkbig2  = new double[ HXVsizeWorkspace ];
   
   // Check for the lapack routines { dgemm_ , daxpy_ , dscal_ , dcopy_ , ddot_ }
   unsigned long long maxVecLength = 0;
   for ( unsigned int irrep = 0; irrep < NumIrreps; irrep++ ){
      if ( getVecLength( irrep ) > maxVecLength ){ maxVecLength = getVecLength( irrep ); }
   }
   const unsigned int max_integer = INT_MAX;
   assert( max_integer >= maxVecLength );
   
}

void CheMPS2::FCI::str2bits(const unsigned int Lval, const unsigned int bitstring, int * bits){

   for (unsigned int bit = 0; bit < Lval; bit++){ bits[ bit ] = ( bitstring & ( 1 << bit ) ) >> bit; }

}

unsigned int CheMPS2::FCI::bits2str(const unsigned int Lval, int * bits){

   unsigned int factor = 1;
   unsigned int result = 0;
   for (unsigned int bit = 0; bit < Lval; bit++){
      result += bits[ bit ] * factor;
      factor *= 2;
   }
   return result;

}

int CheMPS2::FCI::getUpIrrepOfCounter(const int irrep_center, const unsigned long long counter) const{

   int irrep_up = NumIrreps;
   while ( counter < irrep_center_jumps[ irrep_center ][ irrep_up-1 ] ){ irrep_up--; }
   return irrep_up-1;
   
}

void CheMPS2::FCI::getBitsOfCounter(const int irrep_center, const unsigned long long counter, int * bits_up, int * bits_down) const{

   const int localTargetIrrep = getIrrepProduct( irrep_center , TargetIrrep );
   
   const int irrep_up   = getUpIrrepOfCounter( irrep_center , counter );
   const int irrep_down = getIrrepProduct( irrep_up , localTargetIrrep );
   
   const unsigned int count_up   = ( counter - irrep_center_jumps[ irrep_center ][ irrep_up ] ) % numPerIrrep_up[ irrep_up ];
   const unsigned int count_down = ( counter - irrep_center_jumps[ irrep_center ][ irrep_up ] ) / numPerIrrep_up[ irrep_up ];
   
   const unsigned int string_up   = cnt2str_up  [ irrep_up   ][ count_up   ];
   const unsigned int string_down = cnt2str_down[ irrep_down ][ count_down ];
   
   str2bits( L , string_up   , bits_up   );
   str2bits( L , string_down , bits_down );

}

double CheMPS2::FCI::getFCIcoeff(int * bits_up, int * bits_down, double * vector) const{

   const unsigned string_up   = bits2str(L, bits_up  );
   const unsigned string_down = bits2str(L, bits_down);
   
   int irrep_up   = 0;
   int irrep_down = 0;
   for ( unsigned int orb = 0; orb < L; orb++ ){
      if ( bits_up  [ orb ] ){ irrep_up   = getIrrepProduct( irrep_up   , getOrb2Irrep( orb ) ); }
      if ( bits_down[ orb ] ){ irrep_down = getIrrepProduct( irrep_down , getOrb2Irrep( orb ) ); }
   }
   
   const int counter_up   = str2cnt_up  [ irrep_up   ][ string_up   ];
   const int counter_down = str2cnt_down[ irrep_down ][ string_down ];
   
   if (( counter_up == -1 ) || ( counter_down == -1 )){ return 0.0; }
   
   return vector[ irrep_center_jumps[ 0 ][ irrep_up ] + counter_up + numPerIrrep_up[ irrep_up ] * counter_down ];

}

/*void CheMPS2::FCI::CheckHamDEBUG() const{

   const unsigned long long vecLength = getVecLength( 0 );
   
   // Building Ham by HamTimesVec
   double * HamHXV = new double[ vecLength * vecLength ];
   double * workspace = new double[ vecLength ];
   for (unsigned long long count = 0; count < vecLength; count++){
   
      ClearVector( vecLength , workspace );
      workspace[ count ] = 1.0;
      HamTimesVec( workspace , HamHXV + count*vecLength );
   
   }
   
   // Building Diag by HamDiag
   DiagHam( workspace );
   double RMSdiagdifference = 0.0;
   for (unsigned long long row = 0; row < vecLength; row++){
      double diff = workspace[ row ] - HamHXV[ row + vecLength * row ];
      RMSdiagdifference += diff * diff;
   }
   RMSdiagdifference = sqrt( RMSdiagdifference );
   cout << "The RMS difference of DiagHam() and diag(HamHXV) = " << RMSdiagdifference << endl;
   
   // Building Ham by getMatrixElement
   int * work     = new int[ 8 ];
   int * ket_up   = new int[ L ];
   int * ket_down = new int[ L ];
   int * bra_up   = new int[ L ];
   int * bra_down = new int[ L ];
   double RMSconstructiondiff = 0.0;
   for (unsigned long long row = 0; row < vecLength; row++){
      for (unsigned long long col = 0; col < vecLength; col++){
         getBitsOfCounter( 0 , row , bra_up , bra_down );
         getBitsOfCounter( 0 , col , ket_up , ket_down );
         double tempvar = HamHXV[ row + vecLength * col ] - GetMatrixElement( bra_up , bra_down , ket_up , ket_down , work );
         RMSconstructiondiff += tempvar * tempvar;
      }
   }
   cout << "The RMS difference of HamHXV - HamMXELEM = " << RMSconstructiondiff << endl;
   delete [] work;
   delete [] ket_up;
   delete [] ket_down;
   delete [] bra_up;
   delete [] bra_down;
   
   // Building Ham^2 by HamTimesVec
   double * workspace2 = new double[ vecLength ];
   for (unsigned long long count = 0; count < vecLength; count++){
   
      ClearVector( vecLength , workspace );
      workspace[ count ] = 1.0;
      HamTimesVec( workspace , workspace2 );
      HamTimesVec( workspace2 , HamHXV + count*vecLength );
   
   }
   
   // Building diag( Ham^2 ) by DiagHamSquared
   DiagHamSquared( workspace );
   double RMSdiagdifference2 = 0.0;
   for (unsigned long long row = 0; row < vecLength; row++){
      double diff = workspace[ row ] - HamHXV[ row + vecLength * row ];
      RMSdiagdifference2 += diff * diff;
   }
   RMSdiagdifference2 = sqrt( RMSdiagdifference2 );
   cout << "The RMS difference of DiagHamSquared() and diag(HamSquared by HXV) = " << RMSdiagdifference2 << endl;
   
   delete [] workspace2;
   delete [] workspace;
   delete [] HamHXV;

}*/

void CheMPS2::FCI::HamTimesVec(double * input, double * output) const{

   struct timeval start, end;
   gettimeofday(&start, NULL);

   ClearVector( getVecLength( 0 ) , output );

   // P.J. Knowles and N.C. Handy, A new determinant-based full configuration interaction method, Chemical Physics Letters 111 (4-5), 315-321 (1984)
   
   // irrep_center is the center irrep of the ERI : (ij|kl) --> irrep_center = I_i x I_j = I_k x I_l
   for ( unsigned int irrep_center = 0; irrep_center < NumIrreps; irrep_center++ ){
   
      const unsigned long long localVecLength = getVecLength( irrep_center );
      const int localTargetIrrep = getIrrepProduct( TargetIrrep , irrep_center );
      const unsigned int numPairs = irrep_center_num[ irrep_center ];
      const unsigned int * center_crea_orb = irrep_center_crea_orb[ irrep_center ];
      const unsigned int * center_anni_orb = irrep_center_anni_orb[ irrep_center ];
      const unsigned long long * center_jumps    = irrep_center_jumps[ irrep_center ];
      const unsigned long long * zero_jumps      = irrep_center_jumps[ 0 ];

      const unsigned int space_per_vectorpiece = (( numPairs == 0 ) ? HXVsizeWorkspace : (unsigned int) floor(( 1.0 * HXVsizeWorkspace ) / numPairs));
      unsigned int numIterations = localVecLength / space_per_vectorpiece;
      if ( localVecLength > numIterations * space_per_vectorpiece ){ numIterations++; }

      for ( unsigned int iteration = 0; iteration < numIterations; iteration++ ){
      
         const unsigned long long veccounter_start = iteration * space_per_vectorpiece;
         const unsigned long long guess_stop       = ( iteration + 1 ) * space_per_vectorpiece;
         const unsigned long long veccounter_stop  = ( guess_stop > localVecLength ) ? localVecLength : guess_stop;
   
         /******************************************************************************************************************************
          *   First build workbig1[ i<=j + size(i<=j) * veccounter ] = E_{i<=j} + ( 1 - delta_i==j ) E_{j>i} (irrep_center) | input >  *
          ******************************************************************************************************************************/
         const unsigned long long loopsize = numPairs * ( veccounter_stop - veccounter_start );
         #pragma omp parallel for schedule(static)
         for ( unsigned long long loopvariable = 0; loopvariable < loopsize; loopvariable++ ){
         
            const unsigned int pair               = loopvariable % numPairs;
            const unsigned long long veccounter   = veccounter_start + ( loopvariable / numPairs );
            const unsigned int creator            = center_crea_orb[ pair ];
            const unsigned int annihilator        = center_anni_orb[ pair ];
            const int irrep_new_up                = getUpIrrepOfCounter( irrep_center , veccounter );
            const int irrep_new_down              = getIrrepProduct( irrep_new_up , localTargetIrrep );
            const unsigned int count_new_up       = ( veccounter - center_jumps[ irrep_new_up ] ) % numPerIrrep_up[ irrep_new_up ];
            const unsigned int count_new_down     = ( veccounter - center_jumps[ irrep_new_up ] ) / numPerIrrep_up[ irrep_new_up ];
            
            double myResult = 0.0;
            
            {
               // E^{alpha}_{creator <= annihilator}
               const int entry_up = creator + L * ( annihilator + L * count_new_up );
               const int sign_up  = lookup_sign_alpha [ irrep_new_up ][ entry_up ];
               if ( sign_up != 0 ){ // Required for one-electron calculations
                  const int irrep_old_up = lookup_irrep_alpha[ irrep_new_up ][ entry_up ];
                  const int cnt_old_up   = lookup_cnt_alpha  [ irrep_new_up ][ entry_up ];
                  myResult = sign_up * input[ zero_jumps[ irrep_old_up ] + cnt_old_up + numPerIrrep_up[ irrep_old_up ] * count_new_down ];
               }
               
               // E^{beta}_{creator <= annihilator}
               const int entry_down = creator + L * ( annihilator + L * count_new_down );
               const int sign_down  = lookup_sign_beta[ irrep_new_down ][ entry_down ];
               if ( sign_down != 0 ){ // Required for one-electron calculations
                  const int cnt_old_down = lookup_cnt_beta[ irrep_new_down ][ entry_down ];
                  myResult += sign_down * input[ zero_jumps[ irrep_new_up ] + count_new_up + numPerIrrep_up[ irrep_new_up ] * cnt_old_down ];
               }
            }
            
            if ( annihilator > creator ){
               // E^{alpha}_{annihilator > creator}
               const int entry_up = annihilator + L * ( creator + L * count_new_up );
               const int sign_up  = lookup_sign_alpha [ irrep_new_up ][ entry_up ];
               if ( sign_up != 0 ){ // Required for one-electron calculations
                  const int irrep_old_up = lookup_irrep_alpha[ irrep_new_up ][ entry_up ];
                  const int cnt_old_up   = lookup_cnt_alpha  [ irrep_new_up ][ entry_up ];
                  myResult += sign_up * input[ zero_jumps[ irrep_old_up ] + cnt_old_up + numPerIrrep_up[ irrep_old_up ] * count_new_down ];
               }
               
               // E^{beta}_{annihilator > creator}
               const int entry_down = annihilator + L * ( creator + L * count_new_down );
               const int sign_down  = lookup_sign_beta[ irrep_new_down ][ entry_down ];
               if ( sign_down != 0 ){ // Required for one-electron calculations
                  const int cnt_old_down = lookup_cnt_beta[ irrep_new_down ][ entry_down ];
                  myResult += sign_down * input[ zero_jumps[ irrep_new_up ] + count_new_up + numPerIrrep_up[ irrep_new_up ] * cnt_old_down ];
               }
            }
            
            HXVworkbig1[ loopvariable ] = myResult;
            
         }
         
         /************************************************
          *   If irrep_center==0, do the one-body terms  *
          ************************************************/
         if ( irrep_center==0 ){
            for ( unsigned int pair = 0; pair < numPairs; pair++ ){
               HXVworksmall[ pair ] = getGmat( center_crea_orb[ pair ] , center_anni_orb[ pair ] );
            }
            char trans  = 'T';
            char notran = 'N';
            double one  = 1.0;
            int mdim = veccounter_stop - veccounter_start; // Checked "assert( max_integer >= maxVecLength );" at FCI::StartupIrrepCenter()
            int kdim = numPairs;
            int ndim = 1;
            dgemm_( &trans, &notran, &mdim, &ndim, &kdim, &one, HXVworkbig1, &kdim, HXVworksmall, &kdim, &one, output + veccounter_start, &mdim );
         }
         
         /****************************************************************************************************************************
          *   Now build workbig2[ i<=j + size(i<=j) * veccounter] = 0.5 * ( i<=j | k<=l ) * workbig1[ k<=l + size(k<=l) * counter ]  *
          ****************************************************************************************************************************/
         {
            for ( unsigned int pair1 = 0; pair1 < numPairs; pair1++ ){
               for ( unsigned int pair2 = 0; pair2 < numPairs; pair2++ ){
                  HXVworksmall[ pair1 + numPairs * pair2 ]
                     = 0.5 * getERI( center_crea_orb[ pair1 ] , center_anni_orb[ pair1 ] ,
                                     center_crea_orb[ pair2 ] , center_anni_orb[ pair2 ] );
               }
            }
            char notran = 'N';
            double one  = 1.0;
            double zero = 0.0;
            int mdim = numPairs;
            int kdim = numPairs;
            int ndim = veccounter_stop - veccounter_start; // Checked "assert( max_integer >= maxVecLength );" at FCI::StartupIrrepCenter()
            dgemm_( &notran , &notran , &mdim , &ndim , &kdim , &one , HXVworksmall , &mdim , HXVworkbig1 , &kdim , &zero , HXVworkbig2 , &mdim );
         }
         
         /*************************************************************************************************************
          *   Finally do output <-- E_{i<=j} + (1 - delta_{i==j}) E_{j>i} workbig2[ i<=j + size(i<=j) * veccounter ]  *
          *************************************************************************************************************/
         for ( unsigned int pair = 0; pair < numPairs; pair++ ){
         
            const unsigned int orbi = center_crea_orb[ pair ];
            const unsigned int orbj = center_anni_orb[ pair ];

            #pragma omp parallel for schedule(static) // The given E_{i<=j}^{alpha} connects exactly one veccounter with one location_new
            for ( unsigned long long veccounter = veccounter_start; veccounter < veccounter_stop; veccounter++ ){
               const int irrep_old_up            = getUpIrrepOfCounter( irrep_center , veccounter );
               const unsigned int count_old_up   = ( veccounter - center_jumps[ irrep_old_up ] ) % numPerIrrep_up[ irrep_old_up ];
               const int entry_up                = orbj + L * ( orbi + L * count_old_up );
               const int sign_up                 = lookup_sign_alpha[ irrep_old_up ][ entry_up ];
               if ( sign_up != 0 ){ // Required for thread safety
                  const unsigned int count_old_down     = ( veccounter - center_jumps[ irrep_old_up ] ) / numPerIrrep_up[ irrep_old_up ];
                  const int irrep_new_up                = lookup_irrep_alpha[ irrep_old_up ][ entry_up ];
                  const int cnt_new_up                  = lookup_cnt_alpha  [ irrep_old_up ][ entry_up ];
                  const unsigned long long location_new = zero_jumps[ irrep_new_up ] + cnt_new_up + numPerIrrep_up[ irrep_new_up ] * count_old_down;
                  output[ location_new ] += sign_up * HXVworkbig2[ pair + numPairs * ( veccounter - veccounter_start ) ];
               }
            }
            
            #pragma omp parallel for schedule(static) // The given E_{i<=j}^{beta} connects exactly one veccounter with one location_new
            for ( unsigned long long veccounter = veccounter_start; veccounter < veccounter_stop; veccounter++ ){
               const int irrep_old_up            = getUpIrrepOfCounter( irrep_center , veccounter );
               const unsigned int count_old_down = ( veccounter - center_jumps[ irrep_old_up ] ) / numPerIrrep_up[ irrep_old_up ];
               const int entry_down              = orbj + L * ( orbi + L * count_old_down );
               const int irrep_old_down          = getIrrepProduct( irrep_old_up , localTargetIrrep );
               const int sign_down               = lookup_sign_beta[ irrep_old_down ][ entry_down ];
               if ( sign_down != 0 ){ // Required for thread safety
                  const unsigned int count_old_up       = ( veccounter - center_jumps[ irrep_old_up ] ) % numPerIrrep_up[ irrep_old_up ];
                  const int cnt_new_down                = lookup_cnt_beta[ irrep_old_down ][ entry_down ];
                  const unsigned long long location_new = zero_jumps[ irrep_old_up ] + count_old_up + numPerIrrep_up[ irrep_old_up ] * cnt_new_down;
                  output[ location_new ] += sign_down * HXVworkbig2[ pair + numPairs * ( veccounter - veccounter_start ) ];
               }
            }
            
            if ( orbj > orbi ){
            
               #pragma omp parallel for schedule(static) // The given E_{j>i}^{alpha} connects exactly one veccounter with one location_new
               for ( unsigned long long veccounter = veccounter_start; veccounter < veccounter_stop; veccounter++ ){
                  const int irrep_old_up            = getUpIrrepOfCounter( irrep_center , veccounter );
                  const unsigned int count_old_up   = ( veccounter - center_jumps[ irrep_old_up ] ) % numPerIrrep_up[ irrep_old_up ];
                  const int entry_up                = orbi + L * ( orbj + L * count_old_up );
                  const int sign_up                 = lookup_sign_alpha[ irrep_old_up ][ entry_up ];
                  if ( sign_up != 0 ){ // Required for thread safety
                     const unsigned int count_old_down     = ( veccounter - center_jumps[ irrep_old_up ] ) / numPerIrrep_up[ irrep_old_up ];
                     const int irrep_new_up                = lookup_irrep_alpha[ irrep_old_up ][ entry_up ];
                     const int cnt_new_up                  = lookup_cnt_alpha  [ irrep_old_up ][ entry_up ];
                     const unsigned long long location_new = zero_jumps[ irrep_new_up ] + cnt_new_up + numPerIrrep_up[ irrep_new_up ] * count_old_down;
                     output[ location_new ] += sign_up * HXVworkbig2[ pair + numPairs * ( veccounter - veccounter_start ) ];
                  }
               }
               
               #pragma omp parallel for schedule(static) // The given E_{j>i}^{beta} connects exactly one veccounter with one location_new
               for ( unsigned long long veccounter = veccounter_start; veccounter < veccounter_stop; veccounter++ ){
                  const int irrep_old_up            = getUpIrrepOfCounter( irrep_center , veccounter );
                  const unsigned int count_old_down = ( veccounter - center_jumps[ irrep_old_up ] ) / numPerIrrep_up[ irrep_old_up ];
                  const int entry_down              = orbi + L * ( orbj + L * count_old_down );
                  const int irrep_old_down          = getIrrepProduct( irrep_old_up , localTargetIrrep );
                  const int sign_down               = lookup_sign_beta[ irrep_old_down ][ entry_down ];
                  if ( sign_down != 0 ){ // Required for thread safety
                     const unsigned int count_old_up       = ( veccounter - center_jumps[ irrep_old_up ] ) % numPerIrrep_up[ irrep_old_up ];
                     const int cnt_new_down                = lookup_cnt_beta[ irrep_old_down ][ entry_down ];
                     const unsigned long long location_new = zero_jumps[ irrep_old_up ] + count_old_up + numPerIrrep_up[ irrep_old_up ] * cnt_new_down;
                     output[ location_new ] += sign_down * HXVworkbig2[ pair + numPairs * ( veccounter - veccounter_start ) ];
                  }
               }
            }
            
         }
      }
   }
   
   gettimeofday(&end, NULL);
   const double elapsed = (end.tv_sec - start.tv_sec) + 1e-6 * (end.tv_usec - start.tv_usec);
   if ( FCIverbose >= 1 ){ cout << "FCI::HamTimesVec : Wall time = " << elapsed << " seconds" << endl; }

}

void CheMPS2::FCI::apply_excitation( double * orig_vector, double * result_vector, const int crea, const int anni, const int orig_target_irrep ) const{

   const int result_target_irrep          = getIrrepProduct( getIrrepProduct( getOrb2Irrep( crea ) , getOrb2Irrep( anni ) ), orig_target_irrep );
   const int   orig_irrep_center          = getIrrepProduct( TargetIrrep,   orig_target_irrep );
   const int result_irrep_center          = getIrrepProduct( TargetIrrep, result_target_irrep );
   const unsigned long long   orig_length = getVecLength(   orig_irrep_center );
   const unsigned long long result_length = getVecLength( result_irrep_center );
   
   ClearVector( result_length , result_vector );
   
   for ( int result_irrep_up = 0; result_irrep_up < NumIrreps; result_irrep_up++ ){
      const int result_irrep_down = getIrrepProduct( result_irrep_up, result_target_irrep );
      
      // E^{alpha}_{crea,anni}
      #pragma omp parallel for schedule(static)
      for ( int result_count_up = 0; result_count_up < numPerIrrep_up[ result_irrep_up ]; result_count_up++ ){
         const int entry_up = crea + L * ( anni + L * result_count_up );
         const int sign_up  = lookup_sign_alpha[ result_irrep_up ][ entry_up ];
         if ( sign_up != 0 ){
            const int orig_irrep_up = lookup_irrep_alpha[ result_irrep_up ][ entry_up ];
            const int orig_count_up = lookup_cnt_alpha  [ result_irrep_up ][ entry_up ];
            const unsigned long long result_location_base = irrep_center_jumps[ result_irrep_center ][ result_irrep_up ] + result_count_up;
            const unsigned long long   orig_location_base = irrep_center_jumps[   orig_irrep_center ][   orig_irrep_up ] +   orig_count_up;
            const int result_stride = numPerIrrep_up[ result_irrep_up ];
            const int   orig_stride = numPerIrrep_up[   orig_irrep_up ];
            for ( int count_down = 0; count_down < numPerIrrep_down[ result_irrep_down ]; count_down++ ){
               result_vector[ result_location_base + result_stride * count_down ] += sign_up * orig_vector[ orig_location_base + orig_stride * count_down ];
            }
         }
      }
      
      // E^{beta}_{crea,anni}
      #pragma omp parallel for schedule(static)
      for ( int result_count_down = 0; result_count_down < numPerIrrep_down[ result_irrep_down ]; result_count_down++ ){
         const int entry_down = crea + L * ( anni + L * result_count_down );
         const int sign_down  = lookup_sign_beta[ result_irrep_down ][ entry_down ];
         if ( sign_down != 0 ){
            const int orig_count_down = lookup_cnt_beta[ result_irrep_down ][ entry_down ];
            const unsigned long long result_location_base = irrep_center_jumps[ result_irrep_center ][ result_irrep_up ] + numPerIrrep_up[ result_irrep_up ] * result_count_down;
            const unsigned long long   orig_location_base = irrep_center_jumps[   orig_irrep_center ][ result_irrep_up ] + numPerIrrep_up[ result_irrep_up ] *   orig_count_down;
            for ( int count_up = 0; count_up < numPerIrrep_up[ result_irrep_up ]; count_up++ ){
               result_vector[ result_location_base + count_up ] += sign_down * orig_vector[ orig_location_base + count_up ];
            }
         }
      }
      
   }

}

double CheMPS2::FCI::Fill2RDM(double * vector, double * two_rdm) const{

   assert( Nel_up + Nel_down >= 2 );

   struct timeval start, end;
   gettimeofday(&start, NULL);
   
   ClearVector( L*L*L*L, two_rdm );
   const unsigned long long length2 = getVecLength( 0 );
   unsigned long long max_length = 0;
   for ( unsigned int irrep = 0; irrep < NumIrreps; irrep++ ){
      if ( getVecLength( irrep ) > max_length ){ max_length = getVecLength( irrep ); }
   }
   double * workspace1 = new double[ max_length ];
   double * workspace2 = new double[ length2    ];
   
   for ( unsigned int irrep_center1 = 0; irrep_center1 < NumIrreps; irrep_center1++ ){
   
      const unsigned long long length1 = getVecLength( irrep_center1 );
      const int target_irrep1          = getIrrepProduct( TargetIrrep , irrep_center1 );
      
      // Gamma_{ijkl} = < E_ik E_jl > - delta_jk < E_il >
      for ( unsigned int anni1 = 0; anni1 < L; anni1++ ){ // anni1 = l
         for ( unsigned int crea1 = anni1; crea1 < L; crea1++ ){ // crea1 = j >= l
         
            const int irrep_prod1 = getIrrepProduct( getOrb2Irrep( crea1 ) , getOrb2Irrep( anni1 ) );
            if ( irrep_prod1 == irrep_center1 ){
            
               apply_excitation( vector, workspace1, crea1, anni1, TargetIrrep );
               
               if ( irrep_prod1 == 0 ){
                  const double value = FCIddot( length2, workspace1, vector ); // < E_{crea1,anni1} >
                  for ( unsigned int jk = anni1; jk < L; jk++ ){
                     two_rdm[ crea1 + L * ( jk + L * ( jk + L * anni1 ) ) ] -= value;
                  }
               }
               
               for ( unsigned int crea2 = anni1; crea2 < L; crea2++ ){ // crea2 = i >= l
                  for ( unsigned int anni2 = anni1; anni2 < L; anni2++ ){ // anni2 = k >= l
                  
                     const int irrep_prod2 = getIrrepProduct( getOrb2Irrep( crea2 ) , getOrb2Irrep( anni2 ) );
                     if ( irrep_prod2 == irrep_prod1 ){
                     
                        apply_excitation( workspace1, workspace2, crea2, anni2, target_irrep1 );
                        const double value = FCIddot( length2, workspace2, vector ); // < E_{crea2,anni2} E_{crea1,anni1} >
                        two_rdm[ crea2 + L * ( crea1 + L * ( anni2 + L * anni1 ) ) ] += value;
                        
                     }
                  }
               }
            }
         }
      }
   }
   delete [] workspace1;
   delete [] workspace2;
   
   for ( unsigned int anni1 = 0; anni1 < L; anni1++ ){
      for ( unsigned int crea1 = anni1; crea1 < L; crea1++ ){
         const int irrep_prod1 = getIrrepProduct( getOrb2Irrep( crea1 ) , getOrb2Irrep( anni1 ) );
         for ( unsigned int crea2 = anni1; crea2 < L; crea2++ ){
            for ( unsigned int anni2 = anni1; anni2 < L; anni2++ ){
               const int irrep_prod2 = getIrrepProduct( getOrb2Irrep( crea2 ) , getOrb2Irrep( anni2 ) );
               if ( irrep_prod2 == irrep_prod1 ){
                  const double value = two_rdm[ crea2 + L * ( crea1 + L * ( anni2 + L * anni1 ) ) ];
                                       two_rdm[ crea1 + L * ( crea2 + L * ( anni1 + L * anni2 ) ) ] = value;
                                       two_rdm[ anni2 + L * ( anni1 + L * ( crea2 + L * crea1 ) ) ] = value;
                                       two_rdm[ anni1 + L * ( anni2 + L * ( crea1 + L * crea2 ) ) ] = value;
               }
            }
         }
      }
   }
   
   // Calculate the FCI energy
   double FCIenergy = getEconst();
   for ( unsigned int orb1 = 0; orb1 < L; orb1++ ){
      for ( unsigned int orb2 = 0; orb2 < L; orb2++ ){
         double tempvar = 0.0;
         double tempvar2 = 0.0;
         for ( unsigned int orb3 = 0; orb3 < L; orb3++ ){
            tempvar  += getERI( orb1 , orb3 , orb3 , orb2 );
            tempvar2 += two_rdm[ orb1 + L * ( orb3 + L * ( orb2 + L * orb3 ) ) ];
            for ( unsigned int orb4 = 0; orb4 < L; orb4++ ){
               FCIenergy += 0.5 * two_rdm[ orb1 + L * ( orb2 + L * ( orb3 + L * orb4 ) ) ] * getERI( orb1 , orb3 , orb2 , orb4 );
            }
         }
         FCIenergy += ( getGmat( orb1 , orb2 ) + 0.5 * tempvar ) * tempvar2 / ( Nel_up + Nel_down - 1.0);
      }
   }
   
   gettimeofday(&end, NULL);
   const double elapsed = (end.tv_sec - start.tv_sec) + 1e-6 * (end.tv_usec - start.tv_usec);
   if ( FCIverbose > 0 ){ cout << "FCI::Fill2RDM : Wall time = " << elapsed << " seconds" << endl; }
   if ( FCIverbose > 0 ){ cout << "FCI::Fill2RDM : Energy (Ham * 2-RDM)  = " << FCIenergy << endl; }
   return FCIenergy;

}

void CheMPS2::FCI::Fill3RDM(double * vector, double * three_rdm) const{

   assert( Nel_up + Nel_down >= 3 );

   struct timeval start, end;
   gettimeofday(&start, NULL);
   
   /*
      Gamma_{ijk,lmn} = < E_il E_jm E_kn >
                      - delta_kl < E_jm E_in >
                      - delta_jl < E_im E_kn >
                      - delta_km < E_il E_jn >
                      + delta_kl delta_im < E_jn >
                      + delta_jl delta_km < E_in >
   */
   
   ClearVector( L*L*L*L*L*L, three_rdm );
   const unsigned long long length3 = getVecLength( 0 );
   unsigned long long max_length = getVecLength( 0 );
   for ( unsigned int irrep = 1; irrep < NumIrreps; irrep++ ){
      if ( getVecLength( irrep ) > max_length ){ max_length = getVecLength( irrep ); }
   }
   double * workspace1 = new double[ max_length ];
   double * workspace2 = new double[ max_length ];
   double * workspace3 = new double[ length3 ];
   
   for ( unsigned int irrep_center1 = 0; irrep_center1 < NumIrreps; irrep_center1++ ){
   
      const unsigned long long length1 = getVecLength( irrep_center1 );
      const int target_irrep1          = getIrrepProduct( TargetIrrep , irrep_center1 );
      
      for ( unsigned int anni1 = 0; anni1 < L; anni1++ ){ // anni1 = n
         for ( unsigned int crea1 = anni1; crea1 < L; crea1++ ){ // crea1 = k >= n
         
            const int irrep_prod1 = getIrrepProduct( getOrb2Irrep( crea1 ) , getOrb2Irrep( anni1 ) );
            if ( irrep_prod1 == irrep_center1 ){
            
               apply_excitation( vector, workspace1, crea1, anni1, TargetIrrep );
               
               if ( irrep_prod1 == 0 ){
               
                  const double value = FCIddot( length1, workspace1, vector ); // < E_{crea1,anni1} >
                  for ( unsigned int m = anni1; m < L; m++ ){
                     for ( unsigned int l = anni1; l < L; l++ ){
                        three_rdm[ m + L * ( crea1 + L * ( l + L * ( l + L * ( m + L * anni1 ) ) ) ) ] += value; // + delta_kl delta_im < E_jn >
                        three_rdm[ crea1 + L * ( l + L * ( m + L * ( l + L * ( m + L * anni1 ) ) ) ) ] += value; // + delta_jl delta_km < E_in >
                     }
                  }
                  
               }
               
               for ( unsigned int irrep_center2 = 0; irrep_center2 < NumIrreps; irrep_center2++ ){
               
                  const unsigned long long length2 = getVecLength( irrep_center2 );
                  const int target_irrep2          = getIrrepProduct( target_irrep1 , irrep_center2 );
                  const int irrep_center3          = getIrrepProduct( irrep_center1 , irrep_center2 );
               
                  for ( unsigned int crea2 = anni1; crea2 < L; crea2++ ){ // crea2 = j >= n
                     for ( unsigned int anni2 = anni1; anni2 < L; anni2++ ){ // anni2 = m >= n
                     
                        const int irrep_prod2 = getIrrepProduct( getOrb2Irrep( crea2 ) , getOrb2Irrep( anni2 ) );
                        if ( irrep_prod2 == irrep_center2 ){
                        
                           apply_excitation( workspace1, workspace2, crea2, anni2, target_irrep1 );
                           
                           if ( irrep_prod1 == irrep_prod2 ){
                           
                              const double value = FCIddot( length2, workspace2, vector ); // < E_{crea2,anni2} E_{crea1,anni1} >
                              for ( unsigned int orb = anni1; orb < L; orb++ ){
                                 three_rdm[ crea1 + L * ( crea2 + L * ( orb   + L * ( orb   + L * ( anni2 + L * anni1 ) ) ) ) ] -= value; // - delta_kl < E_jm E_in >
                                 three_rdm[ crea2 + L * ( orb   + L * ( crea1 + L * ( orb   + L * ( anni2 + L * anni1 ) ) ) ) ] -= value; // - delta_jl < E_im E_kn >
                                 three_rdm[ crea2 + L * ( crea1 + L * ( orb   + L * ( anni2 + L * ( orb   + L * anni1 ) ) ) ) ] -= value; // - delta_km < E_il E_jn >
                              }
                           
                           }
                           
                           for ( unsigned int crea3 = crea2; crea3 < L; crea3++ ){ // crea3 = i >= j = crea2 >= n = anni1
                              for ( unsigned int anni3 = anni1; anni3 < L; anni3++ ){ // anni3 = l >= n
                              
                                 const int irrep_prod3 = getIrrepProduct( getOrb2Irrep( crea3 ) , getOrb2Irrep( anni3 ) );
                                 if ( irrep_prod3 == irrep_center3 ){
                                 
                                    apply_excitation( workspace2, workspace3, crea3, anni3, target_irrep2 );
                                    
                                    const double value = FCIddot( length3, workspace3, vector ); // < E_{crea3,anni3} E_{crea2,anni2} E_{crea1,anni1} >
                                    three_rdm[ crea3 + L * ( crea2 + L * ( crea1 + L * ( anni3 + L * ( anni2 + L * anni1 ) ) ) ) ] += value; // < E_il E_jm E_kn >
                                    
                                 }
                              }
                           }
                        }
                     }
                  }
               }
            }
         }
      }
   }
   delete [] workspace1;
   delete [] workspace2;
   delete [] workspace3;
   
   // Make 12-fold permutation symmetric
   for ( unsigned int anni1 = 0; anni1 < L; anni1++ ){
      for ( unsigned int crea1 = anni1; crea1 < L; crea1++ ){
         const int irrep_prod1 = getIrrepProduct( getOrb2Irrep( crea1 ) , getOrb2Irrep( anni1 ) ); // Ic1 x Ia1
         for ( unsigned int crea2 = anni1; crea2 < L; crea2++ ){
            const int irrep_prod2 = getIrrepProduct( irrep_prod1 , getOrb2Irrep( crea2 ) ); // Ic1 x Ia1 x Ic2
            for ( unsigned int anni2 = anni1; anni2 < L; anni2++ ){
               const int irrep_prod3 = getIrrepProduct( irrep_prod2 , getOrb2Irrep( anni2 ) ); // Ic1 x Ia1 x Ic2 x Ia2
               for ( unsigned int crea3 = crea2; crea3 < L; crea3++ ){
                  const int irrep_prod4 = getIrrepProduct( irrep_prod3 , getOrb2Irrep( crea3 ) ); // Ic1 x Ia1 x Ic2 x Ia2 x Ic3
                  for ( unsigned int anni3 = anni1; anni3 < L; anni3++ ){
                     if ( irrep_prod4 == getOrb2Irrep( anni3 )){ // Ic1 x Ia1 x Ic2 x Ia2 x Ic3 == Ia3
                     
                        /*      crea3 >= crea2 >= anni1
                           crea1, anni3, anni2 >= anni1  */
                  
   const double value = three_rdm[ crea3 + L * ( crea2 + L * ( crea1 + L * ( anni3 + L * ( anni2 + L * anni1 ) ) ) ) ];
                        three_rdm[ crea2 + L * ( crea3 + L * ( crea1 + L * ( anni2 + L * ( anni3 + L * anni1 ) ) ) ) ] = value;
                        
                        three_rdm[ crea2 + L * ( crea1 + L * ( crea3 + L * ( anni2 + L * ( anni1 + L * anni3 ) ) ) ) ] = value;
                        three_rdm[ crea3 + L * ( crea1 + L * ( crea2 + L * ( anni3 + L * ( anni1 + L * anni2 ) ) ) ) ] = value;
                        
                        three_rdm[ crea1 + L * ( crea3 + L * ( crea2 + L * ( anni1 + L * ( anni3 + L * anni2 ) ) ) ) ] = value;
                        three_rdm[ crea1 + L * ( crea2 + L * ( crea3 + L * ( anni1 + L * ( anni2 + L * anni3 ) ) ) ) ] = value;
                        
                        three_rdm[ anni3 + L * ( anni2 + L * ( anni1 + L * ( crea3 + L * ( crea2 + L * crea1 ) ) ) ) ] = value;
                        three_rdm[ anni2 + L * ( anni3 + L * ( anni1 + L * ( crea2 + L * ( crea3 + L * crea1 ) ) ) ) ] = value;
                        
                        three_rdm[ anni2 + L * ( anni1 + L * ( anni3 + L * ( crea2 + L * ( crea1 + L * crea3 ) ) ) ) ] = value;
                        three_rdm[ anni3 + L * ( anni1 + L * ( anni2 + L * ( crea3 + L * ( crea1 + L * crea2 ) ) ) ) ] = value;
                        
                        three_rdm[ anni1 + L * ( anni3 + L * ( anni2 + L * ( crea1 + L * ( crea3 + L * crea2 ) ) ) ) ] = value;
                        three_rdm[ anni1 + L * ( anni2 + L * ( anni3 + L * ( crea1 + L * ( crea2 + L * crea3 ) ) ) ) ] = value;
                        
                     }
                  }
               }
            }
         }
      }
   }
   
   gettimeofday(&end, NULL);
   const double elapsed = (end.tv_sec - start.tv_sec) + 1e-6 * (end.tv_usec - start.tv_usec);
   if ( FCIverbose > 0 ){ cout << "FCI::Fill3RDM : Wall time = " << elapsed << " seconds" << endl; }

}

double CheMPS2::FCI::CalcSpinSquared(double * vector) const{

   const unsigned long long vecLength = getVecLength( 0 );
   double result = 0.0;
      
   #pragma omp parallel for schedule(static) reduction(+:result)
   for ( unsigned long long counter = 0; counter < vecLength; counter++ ){
      for ( unsigned int orbi = 0; orbi < L; orbi++ ){
         
         const int irrep_up     = getUpIrrepOfCounter( 0 , counter );
         const int irrep_down   = getIrrepProduct( irrep_up , TargetIrrep );
         const int count_up     = ( counter - irrep_center_jumps[ 0 ][ irrep_up ] ) % numPerIrrep_up[ irrep_up ];
         const int count_down   = ( counter - irrep_center_jumps[ 0 ][ irrep_up ] ) / numPerIrrep_up[ irrep_up ];
         
         // Diagonal terms
         const int diff_ii = lookup_sign_alpha[ irrep_up   ][ orbi + L * ( orbi + L * count_up   ) ]
                           - lookup_sign_beta [ irrep_down ][ orbi + L * ( orbi + L * count_down ) ]; //Signed integers so subtracting is OK
         const double vector_at_counter_squared = vector[ counter ] * vector[ counter ];
         result += 0.75 * diff_ii * diff_ii * vector_at_counter_squared;
         
         for ( unsigned int orbj = orbi+1; orbj < L; orbj++ ){
         
            // Sz Sz
            const int diff_jj = lookup_sign_alpha[ irrep_up   ][ orbj + L * ( orbj + L * count_up   ) ]
                              - lookup_sign_beta [ irrep_down ][ orbj + L * ( orbj + L * count_down ) ]; //Signed integers so subtracting is OK
            result += 0.5 * diff_ii * diff_jj * vector_at_counter_squared;
            
            const int irrep_up_bis = getIrrepProduct( irrep_up , getIrrepProduct( getOrb2Irrep( orbi ) , getOrb2Irrep( orbj ) ) );
            
            // - ( a_i,up^+ a_j,up )( a_j,down^+ a_i,down )
            const int entry_down_ji = orbj + L * ( orbi + L * count_down );
            const int sign_down_ji  = lookup_sign_beta [ irrep_down ][ entry_down_ji ];
            const int entry_up_ij   = orbi + L * ( orbj + L * count_up );
            const int sign_up_ij    = lookup_sign_alpha[ irrep_up ][ entry_up_ij ];
            const int sign_product1 = sign_up_ij * sign_down_ji;
            if ( sign_product1 != 0 ){
               const int cnt_down_ji = lookup_cnt_beta[ irrep_down ][ entry_down_ji ];
               const int cnt_up_ij   = lookup_cnt_alpha[ irrep_up ][ entry_up_ij ];
               result -= sign_product1 * vector[ irrep_center_jumps[ 0 ][ irrep_up_bis ] + cnt_up_ij + numPerIrrep_up[ irrep_up_bis ] * cnt_down_ji ] * vector[ counter ];
            }

            // - ( a_j,up^+ a_i,up )( a_i,down^+ a_j,down )
            const int entry_down_ij = orbi + L * ( orbj + L * count_down );
            const int sign_down_ij  = lookup_sign_beta[ irrep_down ][ entry_down_ij ];
            const int entry_up_ji   = orbj + L * ( orbi + L * count_up );
            const int sign_up_ji    = lookup_sign_alpha[ irrep_up ][ entry_up_ji ];
            const int sign_product2 = sign_up_ji * sign_down_ij;
            if ( sign_product2 != 0 ){
               const int cnt_down_ij = lookup_cnt_beta[ irrep_down ][ entry_down_ij ];
               const int cnt_up_ji   = lookup_cnt_alpha[ irrep_up ][ entry_up_ji ];
               result -= sign_product2 * vector[ irrep_center_jumps[ 0 ][ irrep_up_bis ] + cnt_up_ji + numPerIrrep_up[ irrep_up_bis ] * cnt_down_ij ] * vector[ counter ];
            }
         
         }
      }
   }
   
   if ( FCIverbose > 0 ){
      const double intendedS = fabs( 0.5 * Nel_up - 0.5 * Nel_down ); // Be careful with subtracting unsigned integers...
      cout << "FCI::CalcSpinSquared : For intended spin " << intendedS
           << " the measured S(S+1) = " << result << " and intended S(S+1) = " << intendedS * (intendedS + 1.0) << endl;
   }
   return result;

}

void CheMPS2::FCI::DiagHam(double * diag) const{

   const unsigned long long vecLength = getVecLength( 0 );

   #pragma omp parallel
   {

      int * bits_up   = new int[ L ];
      int * bits_down = new int[ L ];
      
      #pragma omp for schedule(static)
      for ( unsigned long long counter = 0; counter < vecLength; counter++ ){
      
         double myResult = 0.0;
         getBitsOfCounter( 0 , counter , bits_up , bits_down ); // Fetch the corresponding bits
         
         for ( unsigned int orb1 = 0; orb1 < L; orb1++ ){
            const int n_tot_orb1 = bits_up[ orb1 ] + bits_down[ orb1 ];
            myResult += n_tot_orb1 * getGmat( orb1 , orb1 );
            for ( unsigned int orb2 = 0; orb2 < L; orb2++ ){
               myResult += 0.5 * n_tot_orb1 * ( bits_up[ orb2 ] + bits_down[ orb2 ] ) * getERI( orb1 , orb1 , orb2 , orb2 );
               myResult += 0.5 * ( n_tot_orb1 - bits_up[ orb1 ] * bits_up[ orb2 ] - bits_down[ orb1 ] * bits_down[ orb2 ] ) * getERI( orb1 , orb2 , orb2 , orb1 );
            }
         }
         
         diag[ counter ] = myResult;
         
      }
      
      delete [] bits_up;
      delete [] bits_down;
      
   }

}


void CheMPS2::FCI::DiagHamSquared(double * output) const{

   struct timeval start, end;
   gettimeofday(&start, NULL);

   const unsigned long long vecLength = getVecLength( 0 );
   
   //
   //   Wick's theorem to evaluate the Hamiltonian squared:
   //
   //      H = g_ij E_ij + 0.5 * (ij|kl) E_ij E_kl
   //
   //      H^2 = g_ij g_kl E_ij E_kl
   //          + 0.5 * [ g_ab (ij|kl) + (ab|ij) g_kl ] E_ab E_ij E_kl
   //          + 0.25 * (ab|cd) * (ij|kl) E_ab E_cd E_ij E_kl
   //
   //      Short illustration of what is being done:
   //
   //          E_ij E_kl = (i,s1)^+ (j,s1) (k,s2)^+ (l,s2)
   //
   //                    = (i,s1)^+ (j,s1) (k,s2)^+ (l,s2)
   //                        |        |      |        |
   //                        ----------      ----------
   //                    + (i,s1)^+ (j,s1) (k,s2)^+ (l,s2)
   //                        |        |      |        |
   //                        |        --------        |
   //                        --------------------------
   //                    = num(i,s1) delta(i,j) num(k,s2) delta(k,l)
   //                    + num(i,s1) delta(i,l) delta(s1,s2) [1-num(k,s2)] delta(k,j)
   //          
   //          g_ij g_kl E_ij E_kl = g_ii g_kk num(i,s1) num(k,s2) + g_ik g_ki num(i,s1) [1-num(k,s2)] delta(s1,s2)
   //
   
   #pragma omp parallel
   {

      int * bits_up    = new int[ L ];
      int * bits_down  = new int[ L ];
      
      double * Jmat       = new double[ L * L ]; // (ij|kk)( n_k,up + n_k,down )
      double * K_reg_up   = new double[ L * L ]; // (ik|kj)( n_k,up )
      double * K_reg_down = new double[ L * L ]; // (ik|kj)( n_k,down )
      double * K_bar_up   = new double[ L * L ]; // (ik|kj)( 1 - n_k,up )
      double * K_bar_down = new double[ L * L ]; // (ik|kj)( 1 - n_k,down )
      
      int * specific_orbs_irrep = new int[ NumIrreps * ( L + 1 ) ];
      for ( unsigned int irrep = 0; irrep < NumIrreps; irrep++ ){
         int count = 0;
         for ( unsigned int orb = 0; orb < L; orb++){
            specific_orbs_irrep[ orb + ( L + 1 ) * irrep ] = 0;
            if ( getOrb2Irrep(orb) == irrep ){
               specific_orbs_irrep[ count + ( L + 1 ) * irrep ] = orb;
               count++;
            }
         }
         specific_orbs_irrep[ L + ( L + 1 ) * irrep ] = count;
      }
      
      #pragma omp for schedule(static)
      for (unsigned long long counter = 0; counter < vecLength; counter++){
      
         getBitsOfCounter( 0 , counter , bits_up , bits_down ); // Fetch the corresponding bits
         
         // Construct the J and K matrices properly
         for ( unsigned int i = 0; i < L; i++ ){
            for ( unsigned int j = i; j < L; j++ ){
               
               double val_J        = 0.0;
               double val_KregUP   = 0.0;
               double val_KregDOWN = 0.0;
               double val_KbarUP   = 0.0;
               double val_KbarDOWN = 0.0;
               
               if ( getOrb2Irrep(i) == getOrb2Irrep(j) ){
                  for ( unsigned int k = 0; k < L; k++ ){
                     const double temp = getERI(i,k,k,j);
                     val_J        += getERI(i,j,k,k) * ( bits_up[k] + bits_down[k] );
                     val_KregUP   += temp * bits_up[k];
                     val_KregDOWN += temp * bits_down[k];
                     val_KbarUP   += temp * ( 1 - bits_up[k] );
                     val_KbarDOWN += temp * ( 1 - bits_down[k] );
                  }
               }
               
               Jmat[ i + L * j ] = val_J;
               Jmat[ j + L * i ] = val_J;
               K_reg_up[ i + L * j ] = val_KregUP;
               K_reg_up[ j + L * i ] = val_KregUP;
               K_reg_down[ i + L * j ] = val_KregDOWN;
               K_reg_down[ j + L * i ] = val_KregDOWN;
               K_bar_up[ i + L * j ] = val_KbarUP;
               K_bar_up[ j + L * i ] = val_KbarUP;
               K_bar_down[ i + L * j ] = val_KbarDOWN;
               K_bar_down[ j + L * i ] = val_KbarDOWN;
               
            }
         }
         
         double temp = 0.0;
         // G[i,i] (n_i,up + n_i,down) + 0.5 * ( J[i,i] (n_i,up + n_i,down) + K_bar_up[i,i] * n_i,up + K_bar_down[i,i] * n_i,down )
         for ( unsigned int i = 0; i < L; i++ ){
            const int num_i = bits_up[i] + bits_down[i];
            temp += getGmat(i, i) * num_i + 0.5 * ( Jmat[ i + L * i ] * num_i + K_bar_up[ i + L * i ]   * bits_up[i]
                                                                              + K_bar_down[ i + L * i ] * bits_down[i] );
         }
         double myResult = temp*temp;
         
         for ( unsigned int p = 0; p < L; p++ ){
            for ( unsigned int q = 0; q < L; q++ ){
               if ( getOrb2Irrep(p) == getOrb2Irrep(q) ){
            
                  const int special_pq         = bits_up[p] * ( 1 - bits_up[q] ) + bits_down[p] * ( 1 - bits_down[q] );
                  const double GplusJ_pq       = getGmat(p, q) + Jmat[ p + L * q ];
                  const double K_cross_pq_up   = ( K_bar_up[ p + L * q ] - K_reg_up[ p + L * q ]     ) * bits_up[p]   * ( 1 - bits_up[q]   );
                  const double K_cross_pq_down = ( K_bar_down[ p + L * q ] - K_reg_down[ p + L * q ] ) * bits_down[p] * ( 1 - bits_down[q] );
                  
                  myResult += ( GplusJ_pq * ( special_pq * GplusJ_pq + K_cross_pq_up + K_cross_pq_down )
                              + 0.25 * ( K_cross_pq_up * K_cross_pq_up + K_cross_pq_down * K_cross_pq_down ) );
                  
               }
            }
         }
         
         /*
         
            Part which can be optimized most still --> For H2O/6-31G takes 82.8 % of time with Intel compiler
            Optimization can in principle be done with lookup tables + dgemm_ as matvec product --> bit of work :-(
         
            For future reference: the quantity which is computed here:
            
               0.5 * (ak|ci) * (ak|ci) * [ n_a,up * (1-n_k,up) + n_a,down * (1-n_k,down) ] * [ n_c,up * (1-n_i,up) + n_c,down * (1-n_i,down) ]
             - 0.5 * (ak|ci) * (ai|ck) * [ n_a,up * n_c,up * (1-n_i,up) * (1-n_k,up) + n_a,down * n_c,down * (1-n_i,down) * (1-n_k,down) ]
         
         */
         for ( unsigned int k = 0; k < L; k++ ){
            if ( bits_up[k] + bits_down[k] < 2 ){
               for ( unsigned int a = 0; a < L; a++ ){
               
                  const int special_ak    = ( bits_up[a]   * ( 1 - bits_up[k]   )
                                            + bits_down[a] * ( 1 - bits_down[k] ) );
                  const int local_ak_up   = bits_up[a]   * ( 1 - bits_up[k]   );
                  const int local_ak_down = bits_down[a] * ( 1 - bits_down[k] );
               
                  if ( ( special_ak > 0 ) || ( local_ak_up > 0 ) || ( local_ak_down > 0 ) ){
                  
                     const int irrep_ak = getIrrepProduct( getOrb2Irrep(a), getOrb2Irrep(k) );
                        
                     for ( unsigned int i = 0; i < L; i++ ){
                        if ( bits_up[i] + bits_down[i] < 2 ){
                  
                           const int offset     = getIrrepProduct( irrep_ak, getOrb2Irrep(i) ) * ( L + 1 );
                           const int bar_i_up   = 1 - bits_up[i];
                           const int bar_i_down = 1 - bits_down[i];
                           const int max_c_cnt  = specific_orbs_irrep[ L + offset ];
                              
                           for ( int c_cnt = 0; c_cnt < max_c_cnt; c_cnt++ ){
                              const int c            = specific_orbs_irrep[ c_cnt + offset ];
                              const int fact_ic_up   = bits_up[c]   * bar_i_up;
                              const int fact_ic_down = bits_down[c] * bar_i_down;
                              const int prefactor1   = ( fact_ic_up + fact_ic_down ) * special_ak;
                              const int prefactor2   = local_ak_up * fact_ic_up + local_ak_down * fact_ic_down;
                              const double eri_akci  = getERI(a, k, c, i);
                              const double eri_aick  = getERI(a, i, c, k);
                              myResult += 0.5 * eri_akci * ( prefactor1 * eri_akci - prefactor2 * eri_aick );
                           }
                        }
                     }
                  }
               }
            }
         }
         
         output[ counter ] = myResult;
      
      }
      
      delete [] bits_up;
      delete [] bits_down;
      
      delete [] Jmat;
      delete [] K_reg_up;
      delete [] K_reg_down;
      delete [] K_bar_up;
      delete [] K_bar_down;
      
      delete [] specific_orbs_irrep;
   
   }
   
   gettimeofday(&end, NULL);
   const double elapsed = (end.tv_sec - start.tv_sec) + 1e-6 * (end.tv_usec - start.tv_usec);
   if ( FCIverbose > 0 ){ cout << "FCI::DiagHamSquared : Wall time = " << elapsed << " seconds" << endl; }

}


unsigned long long CheMPS2::FCI::LowestEnergyDeterminant() const{

   const unsigned long long vecLength = getVecLength( 0 );
   double * energies = new double[ vecLength ];

   // Fetch the Slater determinant energies
   DiagHam( energies );
   
   // Find the determinant with minimum energy
   unsigned long long minEindex = 0;
   for ( unsigned long long count = 1; count < vecLength; count++ ){
      if ( energies[ count ] < energies[ minEindex ] ){
         minEindex = count;
      }
   }
   
   delete [] energies;
   
   return minEindex;

}

double CheMPS2::FCI::GetMatrixElement(int * bits_bra_up, int * bits_bra_down, int * bits_ket_up, int * bits_ket_down, int * work) const{
   
   int count_annih_up   = 0;
   int count_creat_up   = 0;
   int count_annih_down = 0;
   int count_creat_down = 0;
   
   int * annih_up   = work;
   int * creat_up   = work+2;
   int * annih_down = work+4;
   int * creat_down = work+6;
   
   // Find the differences between bra and ket, and store them in the arrays annih/creat
   for (unsigned int orb = 0; orb < L; orb++){
      if ( bits_bra_up[ orb ] != bits_ket_up[ orb ] ){
         if ( bits_ket_up[ orb ] ){ // Electron is annihilated in the ket
            if ( count_annih_up == 2 ){ return 0.0; }
            annih_up[ count_annih_up ] = orb;
            count_annih_up++;
         } else { // Electron is created in the ket
            if ( count_creat_up == 2 ){ return 0.0; }
            creat_up[ count_creat_up ] = orb;
            count_creat_up++;
         }
      }
      if ( bits_bra_down[ orb ] != bits_ket_down[ orb ] ){
         if ( bits_ket_down[ orb ] ){ // Electron is annihilated in the ket
            if ( count_annih_down == 2 ){ return 0.0; }
            annih_down[ count_annih_down ] = orb;
            count_annih_down++;
         } else { // Electron is created in the ket
            if ( count_creat_down == 2 ){ return 0.0; }
            creat_down[ count_creat_down ] = orb;
            count_creat_down++;
         }
      }
   }
   
   // Sanity check: spin symmetry
   if ( count_annih_up   != count_creat_up   ){ return 0.0; }
   if ( count_annih_down != count_creat_down ){ return 0.0; }
   
   // Sanity check: At most 2 annihilators and 2 creators can connect the ket and bra
   if ( count_annih_up + count_annih_down > 2 ){ return 0.0; }
   if ( count_creat_up + count_creat_down > 2 ){ return 0.0; }
   
   if (( count_annih_up == 0 ) && ( count_annih_down == 0 )){ // |bra> == |ket>  -->  copy from DiagHam( )
   
      double result = 0.0;
      for ( unsigned int orb1 = 0; orb1 < L; orb1++ ){
         const int n_tot_orb1 = bits_ket_up[ orb1 ] + bits_ket_down[ orb1 ];
         result += n_tot_orb1 * getGmat( orb1 , orb1 );
         for ( unsigned int orb2 = 0; orb2 < L; orb2++ ){
            result += 0.5 * n_tot_orb1 * ( bits_ket_up[ orb2 ] + bits_ket_down[ orb2 ] ) * getERI( orb1 , orb1 , orb2 , orb2 )
            + 0.5 * ( n_tot_orb1 - bits_ket_up[ orb1 ] * bits_ket_up[ orb2 ] - bits_ket_down[ orb1 ] * bits_ket_down[ orb2 ] ) * getERI( orb1 , orb2 , orb2 , orb1 );
         }
      }
      return result;
      
   }
   
   if (( count_annih_up == 1 ) && ( count_annih_down == 0 )){ // |bra> = a^+_j,up a_l,up |ket>
   
      const int orbj = creat_up[ 0 ];
      const int orbl = annih_up[ 0 ];
   
      double result = getGmat( orbj , orbl );
      for (unsigned int orb1 = 0; orb1 < L; orb1++){
         result += getERI(orbj, orb1, orb1, orbl) * ( 0.5 - bits_ket_up[ orb1 ] ) + getERI(orb1, orb1, orbj, orbl) * ( bits_ket_up[ orb1 ] + bits_ket_down[ orb1 ] );
      }
      int phase = 1;
      if ( orbj < orbl ){ for (int orbital = orbj+1; orbital < orbl; orbital++){ if ( bits_ket_up[ orbital ] ){ phase *= -1; } } }
      if ( orbl < orbj ){ for (int orbital = orbl+1; orbital < orbj; orbital++){ if ( bits_ket_up[ orbital ] ){ phase *= -1; } } }
      return ( result * phase );
   
   }
   
   if (( count_annih_up == 0 ) && ( count_annih_down == 1 )){ // |bra> = a^+_j,down a_l,down |ket>
   
      const int orbj = creat_down[ 0 ];
      const int orbl = annih_down[ 0 ];
   
      double result = getGmat( orbj , orbl );
      for (unsigned int orb1 = 0; orb1 < L; orb1++){
         result += getERI(orbj, orb1, orb1, orbl) * ( 0.5 - bits_ket_down[ orb1 ] ) + getERI(orb1, orb1, orbj, orbl) * ( bits_ket_up[ orb1 ] + bits_ket_down[ orb1 ] );
      }
      int phase = 1;
      if ( orbj < orbl ){ for (int orbital = orbj+1; orbital < orbl; orbital++){ if ( bits_ket_down[ orbital ] ){ phase *= -1; } } }
      if ( orbl < orbj ){ for (int orbital = orbl+1; orbital < orbj; orbital++){ if ( bits_ket_down[ orbital ] ){ phase *= -1; } } }
      return ( result * phase );
   
   }
   
   if (( count_annih_up == 2 ) && ( count_annih_down == 0 )){
   
      // creat and annih are filled in increasing orbital index
      const int orbi = creat_up[ 0 ];
      const int orbj = creat_up[ 1 ];
      const int orbk = annih_up[ 0 ];
      const int orbl = annih_up[ 1 ];
      
      double result = getERI(orbi, orbk, orbj, orbl) - getERI(orbi, orbl, orbj, orbk);
      int phase = 1;
      for (int orbital = orbk+1; orbital < orbl; orbital++){ if ( bits_ket_up[ orbital ] ){ phase *= -1; } } // Fermion phases orbk and orbl measured in the ket
      for (int orbital = orbi+1; orbital < orbj; orbital++){ if ( bits_bra_up[ orbital ] ){ phase *= -1; } } // Fermion phases orbi and orbj measured in the bra
      return ( result * phase );
   
   }
   
   if (( count_annih_up == 0 ) && ( count_annih_down == 2 )){
   
      // creat and annih are filled in increasing orbital index
      const int orbi = creat_down[ 0 ];
      const int orbj = creat_down[ 1 ];
      const int orbk = annih_down[ 0 ];
      const int orbl = annih_down[ 1 ];
      
      double result = getERI(orbi, orbk, orbj, orbl) - getERI(orbi, orbl, orbj, orbk);
      int phase = 1;
      for (int orbital = orbk+1; orbital < orbl; orbital++){ if ( bits_ket_down[ orbital ] ){ phase *= -1; } } // Fermion phases orbk and orbl measured in the ket
      for (int orbital = orbi+1; orbital < orbj; orbital++){ if ( bits_bra_down[ orbital ] ){ phase *= -1; } } // Fermion phases orbi and orbj measured in the bra
      return ( result * phase );
   
   }
   
   if (( count_annih_up == 1 ) && ( count_annih_down == 1 )){
   
      const int orbi = creat_up  [ 0 ];
      const int orbj = creat_down[ 0 ];
      const int orbk = annih_up  [ 0 ];
      const int orbl = annih_down[ 0 ];
      
      double result = getERI(orbi, orbk, orbj, orbl);
      int phase = 1;
      if ( orbi < orbk ){ for (int orbital = orbi+1; orbital < orbk; orbital++){ if ( bits_ket_up  [ orbital ] ){ phase *= -1; } } }
      if ( orbk < orbi ){ for (int orbital = orbk+1; orbital < orbi; orbital++){ if ( bits_ket_up  [ orbital ] ){ phase *= -1; } } }
      if ( orbj < orbl ){ for (int orbital = orbj+1; orbital < orbl; orbital++){ if ( bits_ket_down[ orbital ] ){ phase *= -1; } } }
      if ( orbl < orbj ){ for (int orbital = orbl+1; orbital < orbj; orbital++){ if ( bits_ket_down[ orbital ] ){ phase *= -1; } } }
      return ( result * phase );
   
   }
   
   return 0.0;

}

void CheMPS2::FCI::FCIdcopy(const unsigned long long vecLength, double * origin, double * target){

   int length = vecLength; // Checked "assert( max_integer >= maxVecLength );" at FCI::StartupIrrepCenter()
   int inc = 1;
   dcopy_( &length , origin , &inc , target , &inc );

}

double CheMPS2::FCI::FCIddot(const unsigned long long vecLength, double * vec1, double * vec2){

   int length = vecLength; // Checked "assert( max_integer >= maxVecLength );" at FCI::StartupIrrepCenter()
   int inc = 1;
   return ddot_( &length , vec1 , &inc , vec2 , &inc );

}

double CheMPS2::FCI::FCIfrobeniusnorm(const unsigned long long vecLength, double * vec){

   return sqrt( FCIddot( vecLength , vec , vec ) );

}

void CheMPS2::FCI::FCIdaxpy(const unsigned long long vecLength, const double alpha, double * vec_x, double * vec_y){

   double factor = alpha;
   int length = vecLength; // Checked "assert( max_integer >= maxVecLength );" at FCI::StartupIrrepCenter()
   int inc = 1;
   daxpy_( &length , &factor , vec_x , &inc , vec_y , &inc );

}

void CheMPS2::FCI::FCIdscal(const unsigned long long vecLength, const double alpha, double * vec){

   double factor = alpha;
   int length = vecLength; // Checked "assert( max_integer >= maxVecLength );" at FCI::StartupIrrepCenter()
   int inc = 1;
   dscal_( &length , &factor , vec , &inc );

}

void CheMPS2::FCI::ClearVector(const unsigned long long vecLength, double * vec){

   for ( unsigned long long cnt = 0; cnt < vecLength; cnt++ ){ vec[cnt] = 0.0; }

}

void CheMPS2::FCI::FillRandom(const unsigned long long vecLength, double * vec){

   for ( unsigned long long cnt = 0; cnt < vecLength; cnt++ ){ vec[cnt] = ( ( 2.0 * rand() ) / RAND_MAX ) - 1.0; }

}

double CheMPS2::FCI::GSDavidson(double * inoutput, const int DAVIDSON_NUM_VEC) const{

   const int veclength = getVecLength( 0 ); // Checked "assert( max_integer >= maxVecLength );" at FCI::StartupIrrepCenter()
   const double RTOL   = CheMPS2::HEFF_DAVIDSON_RTOL_BASE * sqrt( 1.0 * veclength );
   
   Davidson deBoskabouter( veclength, DAVIDSON_NUM_VEC, CheMPS2::HEFF_DAVIDSON_NUM_VEC_KEEP, RTOL, CheMPS2::HEFF_DAVIDSON_PRECOND_CUTOFF, false ); // No debug printing for FCI
   double ** whichpointers = new double*[2];
   
   char instruction = deBoskabouter.FetchInstruction( whichpointers );
   assert( instruction == 'A' );
   if ( inoutput != NULL ){ FCIdcopy( veclength, inoutput, whichpointers[0] ); }
   else { FillRandom( veclength, whichpointers[0] ); }
   DiagHam( whichpointers[1] );
   
   instruction = deBoskabouter.FetchInstruction( whichpointers );
   while ( instruction == 'B' ){
      HamTimesVec( whichpointers[0], whichpointers[1] );
      instruction = deBoskabouter.FetchInstruction( whichpointers );
   }
   
   assert( instruction == 'C' );
   if ( inoutput != NULL ){ FCIdcopy( veclength, whichpointers[0], inoutput ); }
   const double FCIenergy = whichpointers[1][0] + getEconst();
   if ( FCIverbose > 1 ){ cout << "FCI::GSDavidson : Required number of matrix-vector multiplications = " << deBoskabouter.GetNumMultiplications() << endl; }
   if ( FCIverbose > 0 ){ cout << "FCI::GSDavidson : Converged ground state energy = " << FCIenergy << endl; }
   delete [] whichpointers;
   return FCIenergy;

}

/*********************************************************************************
 *                                                                               *
 *   Below this block all functions are for the Green's function calculations.   *
 *                                                                               *
 *********************************************************************************/

void CheMPS2::FCI::ActWithNumberOperator(const unsigned int orbIndex, double * resultVector, double * sourceVector) const{

   assert( orbIndex<L );

   int * bits_up    = new int[ L ];
   int * bits_down  = new int[ L ];

   const unsigned long long vecLength = getVecLength( 0 );
   for (unsigned long long counter = 0; counter < vecLength; counter++){
      getBitsOfCounter( 0 , counter , bits_up , bits_down );
      resultVector[ counter ] = ( bits_up[ orbIndex ] + bits_down[ orbIndex ] ) * sourceVector[ counter ];
   }
   
   delete [] bits_up;
   delete [] bits_down;

}

void CheMPS2::FCI::ActWithSecondQuantizedOperator(const char whichOperator, const bool isUp, const unsigned int orbIndex, double * thisVector, const FCI * otherFCI, double * otherVector) const{

   assert( ( whichOperator=='C' ) || ( whichOperator=='A' ) ); //Operator should be a (C) Creator, or (A) Annihilator
   assert( orbIndex<L  );
   assert( L==otherFCI->getL() );

   const unsigned long long vecLength = getVecLength( 0 );

   if ( getTargetIrrep() != getIrrepProduct( otherFCI->getTargetIrrep() , getOrb2Irrep( orbIndex ) )){
      ClearVector( vecLength , thisVector );
      return;
   }

   int * bits_up    = new int[ L ];
   int * bits_down  = new int[ L ];
   
   if (( whichOperator=='C') && ( isUp )){
      for (unsigned long long counter = 0; counter < vecLength; counter++){
         
         getBitsOfCounter( 0 , counter , bits_up , bits_down );
         
         if ( bits_up[ orbIndex ] == 1 ){ // Operator = creator_up
            bits_up[ orbIndex ] = 0;
            int phase = 1;
            for (unsigned int cnt = 0; cnt < orbIndex; cnt++){ if ( bits_up[ cnt ] ){ phase *= -1; } }
            thisVector[ counter ] = phase * otherFCI->getFCIcoeff( bits_up , bits_down , otherVector );
         } else {
            thisVector[ counter ] = 0.0;
         }
         
      }
   }
   
   if (( whichOperator=='C') && ( !(isUp) )){
      const int startphase = (( Nel_up % 2 ) == 0) ? 1 : -1;
      for (unsigned long long counter = 0; counter < vecLength; counter++){

         getBitsOfCounter( 0 , counter , bits_up , bits_down );

         if ( bits_down[ orbIndex ] == 1 ){ // Operator = creator_down
            bits_down[ orbIndex ] = 0;
            int phase = startphase;
            for (unsigned int cnt = 0; cnt < orbIndex; cnt++){ if ( bits_down[ cnt ] ){ phase *= -1; } }
            thisVector[ counter ] = phase * otherFCI->getFCIcoeff( bits_up , bits_down , otherVector );
         } else {
            thisVector[ counter ] = 0.0;
         }

      }
   }
   
   if (( whichOperator=='A') && ( isUp )){
      for (unsigned long long counter = 0; counter < vecLength; counter++){

         getBitsOfCounter( 0 , counter , bits_up , bits_down );
         
         if ( bits_up[ orbIndex ] == 0 ){ // Operator = annihilator_up
            bits_up[ orbIndex ] = 1;
            int phase = 1;
            for (unsigned int cnt = 0; cnt < orbIndex; cnt++){ if ( bits_up[ cnt ] ){ phase *= -1; } }
            thisVector[ counter ] = phase * otherFCI->getFCIcoeff( bits_up , bits_down , otherVector );
         } else {
            thisVector[ counter ] = 0.0;
         }

      }
   }
   
   if (( whichOperator=='A') && ( !(isUp) )){
      const int startphase = (( Nel_up % 2 ) == 0) ? 1 : -1;
      for (unsigned long long counter = 0; counter < vecLength; counter++){

         getBitsOfCounter( 0 , counter , bits_up , bits_down );
         
         if ( bits_down[ orbIndex ] == 0 ){ // Operator = annihilator_down
            bits_down[ orbIndex ] = 1;
            int phase = startphase;
            for (unsigned int cnt = 0; cnt < orbIndex; cnt++){ if ( bits_down[ cnt ] ){ phase *= -1; } }
            thisVector[ counter ] = phase * otherFCI->getFCIcoeff( bits_up , bits_down , otherVector );
         } else {
            thisVector[ counter ] = 0.0;
         }
         
      }
   }
   
   delete [] bits_up;
   delete [] bits_down;

}

void CheMPS2::FCI::CGSolveSystem(const double alpha, const double beta, const double eta, double * RHS, double * RealSol, double * ImagSol, const bool checkError) const{

   const unsigned long long vecLength = getVecLength( 0 );
   
   // Create a few helper arrays
   double * RESID  = new double[ vecLength ];
   double * PVEC   = new double[ vecLength ];
   double * OxPVEC = new double[ vecLength ];
   double * temp   = new double[ vecLength ];
   double * temp2  = new double[ vecLength ];
   double * precon = new double[ vecLength ];
   CGDiagPrecond( alpha , beta , eta , precon , temp );
   
   assert( RealSol != NULL );
   assert( ImagSol != NULL );
   assert( fabs( eta ) > 0.0 );

   /* 
         ( alpha + beta H + I eta ) Solution = RHS
      
      is solved with the conjugate gradient (CG) method. Solution = RealSol + I * ImagSol.
      CG requires a symmetric positive definite operator. Therefore:
      
         precon * [ ( alpha + beta H )^2 + eta^2 ] * precon * SolutionTilde = precon * ( alpha + beta H - I eta ) * RHS
         Solution = precon * SolutionTilde
         
      Clue: Solve for ImagSol first. RealSol is then simply
      
         RealSol = - ( alpha + beta H ) / eta * ImagSol
   */
   
   /**** Solve for ImagSol ****/
   for (unsigned long long cnt = 0; cnt < vecLength; cnt++){ RESID[ cnt ] = - eta * precon[ cnt ] * RHS[ cnt ]; } // RESID = - eta * precon * RHS
   if ( FCIverbose > 1 ){ cout << "FCI::CGSolveSystem : Two-norm of the RHS for the imaginary part = " << FCIfrobeniusnorm( vecLength , RESID ) << endl; }
   FCIdcopy( vecLength, RESID, ImagSol ); // Well educated initial guess for the imaginary part (guess is exact if operator is diagonal)
   CGCoreSolver( alpha, beta, eta, precon, ImagSol, RESID, PVEC, OxPVEC, temp, temp2 ); // RESID contains the RHS of ( precon * Op * precon ) * |x> = |b>
   for (unsigned long long cnt = 0; cnt < vecLength; cnt++){ ImagSol[ cnt ] = precon[ cnt ] * ImagSol[ cnt ]; }
   
   /**** Solve for RealSol ****/
   CGAlphaPlusBetaHAM( -alpha/eta, -beta/eta, ImagSol, RealSol ); // Initial guess RealSol can be obtained from ImagSol
   for (unsigned long long cnt = 0; cnt < vecLength; cnt++){
      if ( fabs( precon[cnt] ) > CheMPS2::HEFF_DAVIDSON_PRECOND_CUTOFF ){
         RealSol[cnt] = RealSol[cnt] / precon[cnt];
      } else {
         RealSol[cnt] = RealSol[cnt] / CheMPS2::HEFF_DAVIDSON_PRECOND_CUTOFF;
      }
   }
   CGAlphaPlusBetaHAM( alpha, beta, RHS, RESID ); // RESID = ( alpha + beta * H ) * RHS
   for (unsigned long long cnt = 0; cnt < vecLength; cnt++){ RESID[ cnt ] = precon[ cnt ] * RESID[ cnt ]; } // RESID = precon * ( alpha + beta * H ) * RHS
   if ( FCIverbose > 1 ){ cout << "FCI::CGSolveSystem : Two-norm of the RHS for the real part = " << FCIfrobeniusnorm( vecLength , RESID ) << endl; }
   CGCoreSolver( alpha, beta, eta, precon, RealSol, RESID, PVEC, OxPVEC, temp, temp2 ); // RESID contains the RHS of ( precon * Op * precon ) * |x> = |b>
   for (unsigned long long cnt = 0; cnt < vecLength; cnt++){ RealSol[ cnt ] = precon[ cnt ] * RealSol[ cnt ]; }
   
   if (( checkError ) && ( FCIverbose > 0 )){
      for (unsigned long long cnt = 0; cnt < vecLength; cnt++){ precon[ cnt ] = 1.0; }
      CGOperator( alpha , beta , eta , precon , RealSol , temp , temp2 , OxPVEC );
      CGAlphaPlusBetaHAM( alpha , beta , RHS , RESID );
      FCIdaxpy( vecLength , -1.0 , RESID , OxPVEC );
      double RMSerror = FCIddot( vecLength , OxPVEC , OxPVEC );
      CGOperator( alpha , beta , eta , precon , ImagSol , temp , temp2 , OxPVEC );
      FCIdaxpy( vecLength , eta , RHS , OxPVEC );
      RMSerror += FCIddot( vecLength , OxPVEC , OxPVEC );
      RMSerror = sqrt( RMSerror );
      cout << "FCI::CGSolveSystem : RMS error when checking the solution (without preconditioner) = " << RMSerror << endl;
   }
   
   // Clean up
   delete [] temp;
   delete [] temp2;
   delete [] RESID;
   delete [] PVEC;
   delete [] OxPVEC;
   delete [] precon;

}

void CheMPS2::FCI::CGCoreSolver(const double alpha, const double beta, const double eta, double * precon, double * Sol, double * RESID, double * PVEC, double * OxPVEC, double * temp, double * temp2) const{

   const unsigned long long vecLength = getVecLength( 0 );
   const double CGRESIDUALTHRESHOLD = 100.0 * CheMPS2::HEFF_DAVIDSON_RTOL_BASE * sqrt( 1.0 * vecLength );
   if ( FCIverbose>1 ){ cout << "FCI::CGCoreSolver : The residual norm for convergence = " << CGRESIDUALTHRESHOLD << endl; }

   /*
      Operator = precon * [ ( alpha + beta * H )^2 + eta^2 ] * precon : positive definite and symmetric
      x_0 = Sol
      p_0 (PVEC) = r_0 (RESID) = b - Operator * x_0
      k (count_k) = 0
   */

   int count_k = 0;
   CGOperator( alpha , beta , eta , precon , Sol , temp , temp2 , OxPVEC ); // O_p_k = Operator * Sol
   FCIdaxpy( vecLength , -1.0 , OxPVEC , RESID );                           // r_0 = b - Operator * x_0
   FCIdcopy( vecLength , RESID , PVEC );                                    // p_0 = r_0
   double rkT_rk = FCIddot( vecLength , RESID , RESID );
   double residualNorm = sqrt( rkT_rk );
   
   while ( residualNorm >= CGRESIDUALTHRESHOLD ){
   
      CGOperator( alpha , beta , eta , precon, PVEC , temp , temp2 , OxPVEC ); // O_p_k = Operator * p_k
      const double alpha_k = rkT_rk / FCIddot( vecLength , PVEC , OxPVEC );    // alpha_k = r_k^T * r_k / ( p_k^T * O_p_k )
      FCIdaxpy( vecLength ,  alpha_k , PVEC   , Sol   );                       // x_{k+1} = x_k + alpha_k * p_k
      FCIdaxpy( vecLength , -alpha_k , OxPVEC , RESID );                       // r_{k+1} = r_k - alpha_k * A * p_k
      const double rkplus1T_rkplus1 = FCIddot( vecLength , RESID , RESID );
      const double beta_k = rkplus1T_rkplus1 / rkT_rk ;                        // beta_k = r_{k+1}^T * r_{k+1} / ( r_k^T * r_k )
      for ( unsigned long long cnt = 0; cnt < vecLength; cnt++ ){
         PVEC[ cnt ] = RESID[ cnt ] + beta_k * PVEC[ cnt ];                    // p_{k+1} = r_{k+1} + beta_k * p_k
      }
      count_k++;
      rkT_rk = rkplus1T_rkplus1;
      residualNorm = sqrt( rkT_rk );
      if ( FCIverbose > 1 ){ cout << "FCI::CGCoreSolver : At step " << count_k << " the residual norm is " << residualNorm << endl; }
      
   }

}

void CheMPS2::FCI::CGAlphaPlusBetaHAM(const double alpha, const double beta, double * in, double * out) const{

   HamTimesVec( in , out );
   const unsigned long long vecLength = getVecLength( 0 );
   const double prefactor = alpha + beta * getEconst(); // HamTimesVec does only the parts with second quantized operators
   for (unsigned long long cnt = 0; cnt < vecLength; cnt++){
      out[ cnt ] = prefactor * in[ cnt ] + beta * out[ cnt ]; // out = ( alpha + beta * H ) * in
   }

}

void CheMPS2::FCI::CGOperator(const double alpha, const double beta, const double eta, double * precon, double * in, double * temp, double * temp2, double * out) const{

   const unsigned long long vecLength = getVecLength( 0 );
   for (unsigned long long cnt = 0; cnt < vecLength; cnt++){
      temp[ cnt ] = precon[ cnt ] * in[ cnt ];                 // temp  = precon * in
   }
   CGAlphaPlusBetaHAM( alpha , beta , temp  , temp2 );         // temp2 = ( alpha + beta * H )   * precon * in
   CGAlphaPlusBetaHAM( alpha , beta , temp2 , out   );         // out   = ( alpha + beta * H )^2 * precon * in
   FCIdaxpy( vecLength , eta*eta , temp , out );               // out   = [ ( alpha + beta * H )^2 + eta*eta ] * precon * in
   for (unsigned long long cnt = 0; cnt < vecLength; cnt++){
      out[ cnt ] = precon[ cnt ] * out[ cnt ];                 // out   = precon * [ ( alpha + beta * H )^2 + eta*eta ] * precon * in
   }

}

void CheMPS2::FCI::CGDiagPrecond(const double alpha, const double beta, const double eta, double * precon, double * workspace) const{

   // With operator = [ ( alpha + beta * H )^2 + eta*eta ] ; precon becomes 1 / sqrt( diag ( operator ) ).
   
   DiagHam( precon );
   DiagHamSquared( workspace );
   
   const unsigned long long vecLength = getVecLength( 0 );
   const double alpha_bis = alpha + beta * getEconst();
   const double factor1 = alpha_bis * alpha_bis + eta * eta;
   const double factor2 = 2 * alpha_bis * beta;
   const double factor3 = beta * beta;
   for (unsigned long long row = 0; row < vecLength; row++){
      const double diagElement = factor1 + factor2 * precon[ row ] + factor3 * workspace[ row ];
      precon[ row ] = 1.0 / sqrt( diagElement );
   }
   
   if ( FCIverbose>1 ){
      double minval = precon[0];
      double maxval = precon[0];
      for (unsigned long long cnt = 1; cnt < vecLength; cnt++){
         if ( precon[ cnt ] > maxval ){ maxval = precon[ cnt ]; }
         if ( precon[ cnt ] < minval ){ minval = precon[ cnt ]; }
      }
      cout << "FCI::CGDiagPrecond : Minimum value of diag[ ( alpha + beta * Ham )^2 + eta^2 ] = " << 1.0/(maxval*maxval) << endl;
      cout << "FCI::CGDiagPrecond : Maximum value of diag[ ( alpha + beta * Ham )^2 + eta^2 ] = " << 1.0/(minval*minval) << endl;
   }

}

void CheMPS2::FCI::RetardedGF(const double omega, const double eta, const unsigned int orb_alpha, const unsigned int orb_beta, const bool isUp, const double GSenergy, double * GSvector, CheMPS2::Hamiltonian * Ham, double * RePartGF, double * ImPartGF) const{

   assert( RePartGF != NULL );
   assert( ImPartGF != NULL );

   // G( omega, alpha, beta, eta ) = < 0 | a_{alpha,spin}  [ omega - Ham + E_0 + I*eta ]^{-1} a^+_{beta,spin} | 0 > (addition amplitude)
   //                              + < 0 | a^+_{beta,spin} [ omega + Ham - E_0 + I*eta ]^{-1} a_{alpha,spin}  | 0 > (removal  amplitude)

   double Realpart, Imagpart;
   RetardedGF_addition(omega, eta, orb_alpha, orb_beta, isUp, GSenergy, GSvector, Ham, &Realpart, &Imagpart);
   RePartGF[0] = Realpart; // Set
   ImPartGF[0] = Imagpart; // Set
   
   RetardedGF_removal( omega, eta, orb_alpha, orb_beta, isUp, GSenergy, GSvector, Ham, &Realpart, &Imagpart);
   RePartGF[0] += Realpart; // Add
   ImPartGF[0] += Imagpart;
   
   if ( FCIverbose>0 ){
      cout << "FCI::RetardedGF : G( omega = " << omega << " ; eta = " << eta << " ; i = " << orb_alpha << " ; j = " << orb_beta << " ) = " << RePartGF[0] << " + I * " << ImPartGF[0] << endl;
      cout << "                  Local density of states (LDOS) = " << - ImPartGF[0] / M_PI << endl;
   }

}

void CheMPS2::FCI::GFmatrix_addition(const double alpha, const double beta, const double eta, int * orbsLeft, const unsigned int numLeft, int * orbsRight, const unsigned int numRight, const bool isUp, double * GSvector, CheMPS2::Hamiltonian * Ham, double * RePartsGF, double * ImPartsGF, double ** TwoRDMreal, double ** TwoRDMimag, double ** TwoRDMadd) const{

   /*
                                                                         1
       GF[i + numLeft * j] = < 0 | a_{orbsLeft[i], spin} -------------------------------- a^+_{orbsRight[j], spin} | 0 >
                                                          [ alpha + beta * Ham + I*eta ]
   */

   // Check whether some stuff is OK
   assert( numLeft  > 0 );
   assert( numRight > 0 );
   for (unsigned int cnt = 0; cnt < numLeft;  cnt++){ unsigned int orbl = orbsLeft[  cnt ]; assert((orbl < L) && (orbl >= 0)); }
   for (unsigned int cnt = 0; cnt < numRight; cnt++){ unsigned int orbr = orbsRight[ cnt ]; assert((orbr < L) && (orbr >= 0)); }
   assert( RePartsGF != NULL );
   assert( ImPartsGF != NULL );
   for ( unsigned int counter = 0; counter < numLeft * numRight; counter++ ){
       RePartsGF[ counter ] = 0.0;
       ImPartsGF[ counter ] = 0.0;
   }
   const unsigned int Lpow4 = L*L*L*L;
   for ( unsigned int cnt = 0; cnt < numRight; cnt++ ){
      if ( TwoRDMreal != NULL ){ for ( unsigned int elem = 0; elem < Lpow4; elem++ ){ TwoRDMreal[ cnt ][ elem ] = 0.0; } }
      if ( TwoRDMimag != NULL ){ for ( unsigned int elem = 0; elem < Lpow4; elem++ ){ TwoRDMimag[ cnt ][ elem ] = 0.0; } }
      if ( TwoRDMadd  != NULL ){ for ( unsigned int elem = 0; elem < Lpow4; elem++ ){  TwoRDMadd[ cnt ][ elem ] = 0.0; } }
   }
   
   const bool isOK = ( isUp ) ? ( getNel_up() < L ) : ( getNel_down() < L ); // The electron can be added
   for ( unsigned int cnt_right = 0; cnt_right < numRight; cnt_right++ ){
   
      const int orbitalRight = orbsRight[ cnt_right ];
      bool matchingIrrep = false;
      for ( unsigned int cnt_left = 0; cnt_left < numLeft; cnt_left++ ){
         if ( getOrb2Irrep( orbsLeft[ cnt_left] ) == getOrb2Irrep( orbitalRight ) ){ matchingIrrep = true; }
      }
      
      if ( isOK && matchingIrrep ){
      
         const unsigned int addNelUP   = getNel_up()   + ((isUp) ? 1 : 0);
         const unsigned int addNelDOWN = getNel_down() + ((isUp) ? 0 : 1);
         const int addIrrep = getIrrepProduct( getTargetIrrep(), getOrb2Irrep( orbitalRight ) );
         
         CheMPS2::FCI additionFCI( Ham, addNelUP, addNelDOWN, addIrrep, maxMemWorkMB, FCIverbose );
         const unsigned long long addVecLength = additionFCI.getVecLength( 0 );
         double * addVector = new double[ addVecLength ];
         additionFCI.ActWithSecondQuantizedOperator( 'C', isUp, orbitalRight, addVector, this, GSvector ); // | addVector > = a^+_right,spin | GSvector >
         
         double * RealPartSolution = new double[ addVecLength ];
         double * ImagPartSolution = new double[ addVecLength ];
         additionFCI.CGSolveSystem( alpha, beta, eta, addVector, RealPartSolution, ImagPartSolution );
         
         if ( TwoRDMreal != NULL ){ additionFCI.Fill2RDM( RealPartSolution, TwoRDMreal[ cnt_right ] ); }
         if ( TwoRDMimag != NULL ){ additionFCI.Fill2RDM( ImagPartSolution, TwoRDMimag[ cnt_right ] ); }
         if ( TwoRDMadd  != NULL ){ additionFCI.Fill2RDM( addVector,         TwoRDMadd[ cnt_right ] ); }
         
         for ( unsigned int cnt_left = 0; cnt_left < numLeft; cnt_left++ ){
            const int orbitalLeft = orbsLeft[ cnt_left ];
            if ( getOrb2Irrep( orbitalLeft ) == getOrb2Irrep( orbitalRight ) ){
               additionFCI.ActWithSecondQuantizedOperator( 'C', isUp, orbitalLeft, addVector, this, GSvector ); // | addVector > = a^+_left,spin | GSvector >
               RePartsGF[ cnt_left + numLeft * cnt_right ] = FCIddot( addVecLength, addVector, RealPartSolution );
               ImPartsGF[ cnt_left + numLeft * cnt_right ] = FCIddot( addVecLength, addVector, ImagPartSolution );
            }
         }
         
         delete [] RealPartSolution;
         delete [] ImagPartSolution;
         delete [] addVector;
       
      }
   }

}

void CheMPS2::FCI::RetardedGF_addition(const double omega, const double eta, const unsigned int orb_alpha, const unsigned int orb_beta, const bool isUp, const double GSenergy, double * GSvector, CheMPS2::Hamiltonian * Ham, double * RePartGF, double * ImPartGF, double * TwoRDMreal, double * TwoRDMimag, double * TwoRDMadd) const{

   // Addition amplitude < 0 | a_{alpha, spin} [ omega - Ham + E_0 + I*eta ]^{-1} a^+_{beta, spin} | 0 >
   
   double ** TwoRDMreal_wrap = NULL; if ( TwoRDMreal != NULL ){ TwoRDMreal_wrap = new double*[1]; TwoRDMreal_wrap[0] = TwoRDMreal; }
   double ** TwoRDMimag_wrap = NULL; if ( TwoRDMimag != NULL ){ TwoRDMimag_wrap = new double*[1]; TwoRDMimag_wrap[0] = TwoRDMimag; }
   double **  TwoRDMadd_wrap = NULL; if (  TwoRDMadd != NULL ){  TwoRDMadd_wrap = new double*[1];  TwoRDMadd_wrap[0] = TwoRDMadd;  }
   
   int orb_left  = orb_alpha;
   int orb_right = orb_beta;
   
   GFmatrix_addition( omega + GSenergy, -1.0, eta, &orb_left, 1, &orb_right, 1, isUp, GSvector, Ham, RePartGF, ImPartGF, TwoRDMreal_wrap, TwoRDMimag_wrap, TwoRDMadd_wrap );
   
   if ( TwoRDMreal != NULL ){ delete [] TwoRDMreal_wrap; }
   if ( TwoRDMimag != NULL ){ delete [] TwoRDMimag_wrap; }
   if (  TwoRDMadd != NULL ){ delete [] TwoRDMadd_wrap;  }

}

void CheMPS2::FCI::GFmatrix_removal(const double alpha, const double beta, const double eta, int * orbsLeft, const unsigned int numLeft, int * orbsRight, const unsigned int numRight, const bool isUp, double * GSvector, CheMPS2::Hamiltonian * Ham, double * RePartsGF, double * ImPartsGF, double ** TwoRDMreal, double ** TwoRDMimag, double ** TwoRDMrem) const{

   /*
                                                                           1
       GF[i + numLeft * j] = < 0 | a^+_{orbsLeft[i], spin} -------------------------------- a_{orbsRight[j], spin} | 0 >
                                                            [ alpha + beta * Ham + I*eta ]
   */
   
   // Check whether some stuff is OK
   assert( numLeft  > 0 );
   assert( numRight > 0 );
   for (unsigned int cnt = 0; cnt < numLeft;  cnt++){ unsigned int orbl = orbsLeft [ cnt ]; assert((orbl < L) && (orbl >= 0)); }
   for (unsigned int cnt = 0; cnt < numRight; cnt++){ unsigned int orbr = orbsRight[ cnt ]; assert((orbr < L) && (orbr >= 0)); }
   assert( RePartsGF != NULL );
   assert( ImPartsGF != NULL );
   for ( unsigned int counter = 0; counter < numLeft * numRight; counter++ ){
       RePartsGF[ counter ] = 0.0;
       ImPartsGF[ counter ] = 0.0;
   }
   const unsigned int Lpow4 = L*L*L*L;
   for ( unsigned int cnt = 0; cnt < numRight; cnt++ ){
      if ( TwoRDMreal != NULL ){ for ( unsigned int elem = 0; elem < Lpow4; elem++ ){ TwoRDMreal[ cnt ][ elem ] = 0.0; } }
      if ( TwoRDMimag != NULL ){ for ( unsigned int elem = 0; elem < Lpow4; elem++ ){ TwoRDMimag[ cnt ][ elem ] = 0.0; } }
      if ( TwoRDMrem  != NULL ){ for ( unsigned int elem = 0; elem < Lpow4; elem++ ){  TwoRDMrem[ cnt ][ elem ] = 0.0; } }
   }
   
   const bool isOK = ( isUp ) ? ( getNel_up() > 0 ) : ( getNel_down() > 0 ); // The electron can be removed
   for ( unsigned int cnt_right = 0; cnt_right < numRight; cnt_right++ ){
   
      const int orbitalRight = orbsRight[ cnt_right ];
      bool matchingIrrep = false;
      for ( unsigned int cnt_left = 0; cnt_left < numLeft; cnt_left++ ){
         if ( getOrb2Irrep( orbsLeft[ cnt_left] ) == getOrb2Irrep( orbitalRight ) ){ matchingIrrep = true; }
      }
      
      if ( isOK && matchingIrrep ){
      
         const unsigned int removeNelUP   = getNel_up()   - ((isUp) ? 1 : 0);
         const unsigned int removeNelDOWN = getNel_down() - ((isUp) ? 0 : 1);
         const int removeIrrep = getIrrepProduct( getTargetIrrep(), getOrb2Irrep( orbitalRight ) );
         
         CheMPS2::FCI removalFCI( Ham, removeNelUP, removeNelDOWN, removeIrrep, maxMemWorkMB, FCIverbose );
         const unsigned long long removeVecLength = removalFCI.getVecLength( 0 );
         double * removeVector = new double[ removeVecLength ];
         removalFCI.ActWithSecondQuantizedOperator( 'A', isUp, orbitalRight, removeVector, this, GSvector ); // | removeVector > = a_right,spin | GSvector >
         
         double * RealPartSolution = new double[ removeVecLength ];
         double * ImagPartSolution = new double[ removeVecLength ];
         removalFCI.CGSolveSystem( alpha, beta, eta, removeVector, RealPartSolution, ImagPartSolution );
         
         if ( TwoRDMreal != NULL ){ removalFCI.Fill2RDM( RealPartSolution, TwoRDMreal[ cnt_right ] ); }
         if ( TwoRDMimag != NULL ){ removalFCI.Fill2RDM( ImagPartSolution, TwoRDMimag[ cnt_right ] ); }
         if ( TwoRDMrem  != NULL ){ removalFCI.Fill2RDM( removeVector,      TwoRDMrem[ cnt_right ] ); }
         
         for ( unsigned int cnt_left = 0; cnt_left < numLeft; cnt_left++ ){
            const int orbitalLeft = orbsLeft[ cnt_left ];
            if ( getOrb2Irrep( orbitalLeft ) == getOrb2Irrep( orbitalRight ) ){
               removalFCI.ActWithSecondQuantizedOperator( 'A', isUp, orbitalLeft, removeVector, this, GSvector ); // | removeVector > = a_left,spin | GSvector >
               RePartsGF[ cnt_left + numLeft * cnt_right ] = FCIddot( removeVecLength, removeVector, RealPartSolution );
               ImPartsGF[ cnt_left + numLeft * cnt_right ] = FCIddot( removeVecLength, removeVector, ImagPartSolution );
            }
         }
         
         delete [] RealPartSolution;
         delete [] ImagPartSolution;
         delete [] removeVector;
       
      }
   }

}

void CheMPS2::FCI::RetardedGF_removal(const double omega, const double eta, const unsigned int orb_alpha, const unsigned int orb_beta, const bool isUp, const double GSenergy, double * GSvector, CheMPS2::Hamiltonian * Ham, double * RePartGF, double * ImPartGF, double * TwoRDMreal, double * TwoRDMimag, double * TwoRDMrem) const{

   // Removal amplitude < 0 | a^+_{beta, spin} [ omega + Ham - E_0 + I*eta ]^{-1} a_{alpha, spin} | 0 >
   
   double ** TwoRDMreal_wrap = NULL; if ( TwoRDMreal != NULL ){ TwoRDMreal_wrap = new double*[1]; TwoRDMreal_wrap[0] = TwoRDMreal; }
   double ** TwoRDMimag_wrap = NULL; if ( TwoRDMimag != NULL ){ TwoRDMimag_wrap = new double*[1]; TwoRDMimag_wrap[0] = TwoRDMimag; }
   double **  TwoRDMrem_wrap = NULL; if (  TwoRDMrem != NULL ){  TwoRDMrem_wrap = new double*[1];  TwoRDMrem_wrap[0] = TwoRDMrem;  }
   
   int orb_left  = orb_beta;
   int orb_right = orb_alpha;
   
   // orb_alpha = orb_right in this case !!
   GFmatrix_removal( omega - GSenergy, 1.0, eta, &orb_left, 1, &orb_right, 1, isUp, GSvector, Ham, RePartGF, ImPartGF, TwoRDMreal_wrap, TwoRDMimag_wrap, TwoRDMrem_wrap );
   
   if ( TwoRDMreal != NULL ){ delete [] TwoRDMreal_wrap; }
   if ( TwoRDMimag != NULL ){ delete [] TwoRDMimag_wrap; }
   if (  TwoRDMrem != NULL ){ delete [] TwoRDMrem_wrap;  }

}

void CheMPS2::FCI::DensityResponseGF(const double omega, const double eta, const unsigned int orb_alpha, const unsigned int orb_beta, const double GSenergy, double * GSvector, double * RePartGF, double * ImPartGF) const{

   assert( RePartGF != NULL );
   assert( ImPartGF != NULL );

   // X( omega, alpha, beta, eta ) = < 0 | ( n_alpha - <0| n_alpha |0> ) [ omega - Ham + E_0 + I*eta ]^{-1} ( n_beta  - <0| n_beta  |0> ) | 0 > (forward  amplitude)
   //                              - < 0 | ( n_beta  - <0| n_beta  |0> ) [ omega + Ham - E_0 + I*eta ]^{-1} ( n_alpha - <0| n_alpha |0> ) | 0 > (backward amplitude)
   
   double Realpart, Imagpart;
   DensityResponseGF_forward( omega, eta, orb_alpha, orb_beta, GSenergy, GSvector, &Realpart, &Imagpart);
   RePartGF[0] = Realpart; // Set
   ImPartGF[0] = Imagpart; // Set
   
   DensityResponseGF_backward(omega, eta, orb_alpha, orb_beta, GSenergy, GSvector, &Realpart, &Imagpart);
   RePartGF[0] -= Realpart; // Subtract !!!
   ImPartGF[0] -= Imagpart; // Subtract !!!

   if ( FCIverbose>0 ){
      cout << "FCI::DensityResponseGF : X( omega = " << omega << " ; eta = " << eta << " ; i = " << orb_alpha << " ; j = " << orb_beta << " ) = " << RePartGF[0] << " + I * " << ImPartGF[0] << endl;
      cout << "                         Local density-density response (LDDR) = " << - ImPartGF[0] / M_PI << endl;
   }

}

void CheMPS2::FCI::DensityResponseGF_forward(const double omega, const double eta, const unsigned int orb_alpha, const unsigned int orb_beta, const double GSenergy, double * GSvector, double * RePartGF, double * ImPartGF, double * TwoRDMreal, double * TwoRDMimag, double * TwoRDMdens) const{

   // Forward amplitude: < 0 | ( n_alpha - <0| n_alpha |0> ) [ omega - Ham + E_0 + I*eta ]^{-1} ( n_beta  - <0| n_beta  |0> ) | 0 >
   
   assert( ( orb_alpha<L ) && ( orb_beta<L ) ); // Orbital indices within bound
   assert( RePartGF != NULL );
   assert( ImPartGF != NULL );
   
   const unsigned long long vecLength = getVecLength( 0 );
   double * densityAlphaVector = new double[ vecLength ];
   double * densityBetaVector  = ( orb_alpha == orb_beta ) ? densityAlphaVector : new double[ vecLength ];
   ActWithNumberOperator( orb_alpha , densityAlphaVector , GSvector );             // densityAlphaVector = n_alpha |0>
   const double n_alpha_0 = FCIddot( vecLength , densityAlphaVector , GSvector );  // <0| n_alpha |0>
   FCIdaxpy( vecLength , -n_alpha_0 , GSvector , densityAlphaVector );             // densityAlphaVector = ( n_alpha - <0| n_alpha |0> ) |0>
   if ( orb_alpha != orb_beta ){
      ActWithNumberOperator( orb_beta , densityBetaVector , GSvector );            // densityBetaVector = n_beta |0>
      const double n_beta_0 = FCIddot( vecLength , densityBetaVector , GSvector ); // <0| n_beta |0>
      FCIdaxpy( vecLength , -n_beta_0 , GSvector , densityBetaVector );            // densityBetaVector = ( n_beta - <0| n_beta |0> ) |0>
   }

   double * RealPartSolution = new double[ vecLength ];
   double * ImagPartSolution = new double[ vecLength ];
   CGSolveSystem( omega + GSenergy , -1.0 , eta , densityBetaVector , RealPartSolution , ImagPartSolution );
   if ( TwoRDMreal != NULL ){ Fill2RDM( RealPartSolution , TwoRDMreal ); } // Sets the TwoRDMreal
   RePartGF[0] = FCIddot( vecLength , densityAlphaVector , RealPartSolution );
   delete [] RealPartSolution;
   if ( TwoRDMimag != NULL ){ Fill2RDM( ImagPartSolution , TwoRDMimag ); } // Sets the TwoRDMimag
   ImPartGF[0] = FCIddot( vecLength , densityAlphaVector , ImagPartSolution );
   delete [] ImagPartSolution;

   if ( TwoRDMdens != NULL ){ Fill2RDM( densityBetaVector , TwoRDMdens ); } // Sets the TwoRDMdens
   if ( orb_alpha != orb_beta ){ delete [] densityBetaVector; }
   delete [] densityAlphaVector;

}

void CheMPS2::FCI::DensityResponseGF_backward(const double omega, const double eta, const unsigned int orb_alpha, const unsigned int orb_beta, const double GSenergy, double * GSvector, double * RePartGF, double * ImPartGF, double * TwoRDMreal, double * TwoRDMimag, double * TwoRDMdens) const{

   // Backward amplitude: < 0 | ( n_beta  - <0| n_beta  |0> ) [ omega + Ham - E_0 + I*eta ]^{-1} ( n_alpha - <0| n_alpha |0> ) | 0 >
   
   assert( ( orb_alpha<L ) && ( orb_beta<L ) ); // Orbital indices within bound
   assert( RePartGF != NULL );
   assert( ImPartGF != NULL );
   
   const unsigned long long vecLength = getVecLength( 0 );
   double * densityAlphaVector = new double[ vecLength ];
   double * densityBetaVector  = ( orb_alpha == orb_beta ) ? densityAlphaVector : new double[ vecLength ];
   ActWithNumberOperator( orb_alpha , densityAlphaVector , GSvector );             // densityAlphaVector = n_alpha |0>
   const double n_alpha_0 = FCIddot( vecLength , densityAlphaVector , GSvector );  // <0| n_alpha |0>
   FCIdaxpy( vecLength , -n_alpha_0 , GSvector , densityAlphaVector );             // densityAlphaVector = ( n_alpha - <0| n_alpha |0> ) |0>
   if ( orb_alpha != orb_beta ){
      ActWithNumberOperator( orb_beta , densityBetaVector , GSvector );            // densityBetaVector = n_beta |0>
      const double n_beta_0 = FCIddot( vecLength , densityBetaVector , GSvector ); // <0| n_beta |0>
      FCIdaxpy( vecLength , -n_beta_0 , GSvector , densityBetaVector );            // densityBetaVector = ( n_beta - <0| n_beta |0> ) |0>
   }

   double * RealPartSolution = new double[ vecLength ];
   double * ImagPartSolution = new double[ vecLength ];
   CGSolveSystem( omega - GSenergy , 1.0 , eta , densityAlphaVector , RealPartSolution , ImagPartSolution );
   if ( TwoRDMreal != NULL ){ Fill2RDM( RealPartSolution , TwoRDMreal ); } // Sets the TwoRDMreal
   RePartGF[0] = FCIddot( vecLength , densityBetaVector , RealPartSolution );
   delete [] RealPartSolution;
   if ( TwoRDMimag != NULL ){ Fill2RDM( ImagPartSolution , TwoRDMimag ); } // Sets the TwoRDMimag
   ImPartGF[0] = FCIddot( vecLength , densityBetaVector , ImagPartSolution );
   delete [] ImagPartSolution;

   if ( TwoRDMdens != NULL ){ Fill2RDM( densityAlphaVector , TwoRDMdens ); } // Sets the TwoRDMdens
   if ( orb_alpha != orb_beta ){ delete [] densityBetaVector; }
   delete [] densityAlphaVector;

}


