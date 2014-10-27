/*
    -- MAGMA (version 1.1) --
       Univ. of Tennessee, Knoxville
       Univ. of California, Berkeley
       Univ. of Colorado, Denver
       @date

       @precisions normal z -> s d c
*/

#include "common_magma.h"
#include "trace.h"
#define PRECISION_z

/* ================================================================================ */
extern "C" void
magmablas_zlascl_2x2(
    magma_type_t type, magma_trans_t trans, magma_int_t m, 
    magmaDoubleComplex *dW, magma_int_t lddw,
    magmaDoubleComplex *dA, magma_int_t ldda,
    magma_int_t *info );
extern "C" void
magmablas_zlacpy_cnjg(
    magma_int_t n, magmaDoubleComplex *dA1T, magma_int_t lda1,
    magmaDoubleComplex *dA2T, magma_int_t lda2);
/* ================================================================================ */

/**
    Purpose
    =======

    ZLAHEF computes a partial factorization of a complex Hermitian
    matrix A using the Bunch-Kaufman diagonal pivoting method. The
    partial factorization has the form:

    A  =  ( I  U12 ) ( A11  0  ) (  I    0   )  if UPLO = 'U', or:
          ( 0  U22 ) (  0   D  ) ( U12' U22' )

    A  =  ( L11  0 ) (  D   0  ) ( L11' L21' )  if UPLO = 'L'
          ( L21  I ) (  0  A22 ) (  0    I   )

    where the order of D is at most NB. The actual order is returned in
    the argument KB, and is either NB or NB-1, or N if N <= NB.
    Note that U' denotes the conjugate transpose of U.

    ZLAHEF is an auxiliary routine called by ZHETRF. It uses blocked code
    (calling Level 3 BLAS) to update the submatrix A11 (if UPLO = 'U') or
    A22 (if UPLO = 'L').

    Arguments
    ---------
    @param[in]
    UPLO    CHARACTER
            Specifies whether the upper or lower triangular part of the
            Hermitian matrix A is stored:
      -     = 'U':  Upper triangular
      -     = 'L':  Lower triangular

    @param[in]
    N       INTEGER
            The order of the matrix A.  N >= 0.

    @param[in]
    NB      INTEGER
            The maximum number of columns of the matrix A that should be
            factored.  NB should be at least 2 to allow for 2-by-2 pivot
            blocks.

    @param[out]
    KB      INTEGER
            The number of columns of A that were actually factored.
            KB is either NB-1 or NB, or N if N <= NB.

    @param[in,out]
    A       COMPLEX*16 array, dimension (LDA,N)
            On entry, the Hermitian matrix A.  If UPLO = 'U', the leading
            n-by-n upper triangular part of A contains the upper
            triangular part of the matrix A, and the strictly lower
            triangular part of A is not referenced.  If UPLO = 'L', the
            leading n-by-n lower triangular part of A contains the lower
            triangular part of the matrix A, and the strictly upper
            triangular part of A is not referenced.
            On exit, A contains details of the partial factorization.

    @param[in]
    LDA     INTEGER
            The leading dimension of the array A.  LDA >= max(1,N).

    @param[out]
    ipiv    INTEGER array, dimension (N)
            Details of the interchanges and the block structure of D.
            If UPLO = 'U', only the last KB elements of ipiv are set;
            if UPLO = 'L', only the first KB elements are set.
    \n
            If ipiv(k) > 0, then rows and columns k and ipiv(k) were
            interchanged and D(k,k) is a 1-by-1 diagonal block.
            If UPLO = 'U' and ipiv(k) = ipiv(k-1) < 0, then rows and
            columns k-1 and -ipiv(k) were interchanged and D(k-1:k,k-1:k)
            is a 2-by-2 diagonal block.  If UPLO = 'L' and ipiv(k) =
            ipiv(k+1) < 0, then rows and columns k+1 and -ipiv(k) were
            interchanged and D(k:k+1,k:k+1) is a 2-by-2 diagonal block.
 
    @param[out]
    W       (workspace) COMPLEX*16 array, dimension (LDW,NB)
 
    @param[in]
    LDW     INTEGER
            The leading dimension of the array W.  LDW >= max(1,N).

    @param[out]
    INFO    INTEGER
      -     = 0: successful exit
      -     > 0: if INFO = k, D(k,k) is exactly zero.  The factorization
                 has been completed, but the block diagonal matrix D is
                 exactly singular.
  
    @ingroup magma_zhetrf_comp
    ********************************************************************/
extern "C" magma_int_t
magma_zlahef_gpu(
    magma_uplo_t uplo, magma_int_t n, magma_int_t nb, magma_int_t *kb, 
    magmaDoubleComplex *hA, magma_int_t lda,  
    magmaDoubleComplex *dA, magma_int_t ldda, magma_int_t *ipiv, 
    magmaDoubleComplex *dW, magma_int_t lddw, 
    magma_queue_t stream[], magma_event_t event[],
    magma_int_t *info) 
{
    /* .. Parameters .. */
    double d_one   = 1.0;
    double d_zero  = 0.0;
    double d_eight = 8.0;
    double d_seven = 7.0;
    magmaDoubleComplex c_one  =  MAGMA_Z_ONE;
    magmaDoubleComplex c_mone = -MAGMA_Z_ONE;
    magma_int_t upper = (uplo == MagmaUpper);
    magma_int_t ione = 1;
  
    /* .. Local Scalars .. */
    magma_int_t imax, jmax, kk, kp, kstep, iinfo;
    double   abs_akk, alpha, colmax, R1, rowmax, T;
    magmaDoubleComplex D11, D21, D22, Zimax, Z;

    #define dA(i, j)  (dA[(j)*ldda  + (i)])
    #define dW(i, j)  (dW[(j)*lddw  + (i)])
    #define  A(i, j)  (hA[(j)*lda   + (i)])

    /* .. Executable Statements .. */
    *info = 0;

    /* Initialize alpha for use in choosing pivot block size. */
    alpha = ( d_one+sqrt( d_seven ) ) / d_eight;

    if( upper ) {
/*
*        Factorize the trailing columns of A using the upper triangle
*        of A and working backwards, and compute the matrix W = U12*D
*        for use in updating A11 (note that conjg(W) is actually stored)
*
*        K is the main loop index, decreasing from N in steps of 1 or 2
*
*        KW is the column of W which corresponds to column K of A
*
         K = N
   10    CONTINUE
         KW = NB + K - N
*
*        Exit from loop
*
         IF( ( K.LE.N-NB+1 .AND. NB.LT.N ) .OR. K.LT.1 )
     $      GO TO 30
*
*        Copy column K of A to column KW of W and update it
*
         CALL ZCOPY( K-1, A( 1, K ), 1, W( 1, KW ), 1 )
         W( K, KW ) = DBLE( A( K, K ) )
         IF( K.LT.N ) THEN
            CALL ZGEMV( 'No transpose', K, N-K, -CONE, A( 1, K+1 ), LDA,
     $                  W( K, KW+1 ), LDW, CONE, W( 1, KW ), 1 )
            W( K, KW ) = DBLE( W( K, KW ) )
         END IF
*
         kstep = 1
*
*        Determine rows and columns to be interchanged and whether
*        a 1-by-1 or 2-by-2 pivot block will be used
*
         abs_akk = ABS( DBLE( W( K, KW ) ) )
*
*        imax is the row-index of the largest off-diagonal element in
*        column K, and colmax is its absolute value
*
         IF( K.GT.1 ) THEN
            imax = IZAMAX( K-1, W( 1, KW ), 1 )
            colmax = CABS1( W( imax, KW ) )
         ELSE
            colmax = ZERO
         END IF
*
         IF( MAX( abs_akk, colmax ).EQ.ZERO ) THEN
*
*           Column K is zero: set INFO and continue
*
            IF( INFO.EQ.0 )
     $         INFO = K
            kp = K
            A( K, K ) = DBLE( A( K, K ) )
         ELSE
            IF( abs_akk.GE.alpha*colmax ) THEN
*
*              no interchange, use 1-by-1 pivot block
*
               kp = K
            ELSE
*
*              Copy column imax to column KW-1 of W and update it
*
               CALL ZCOPY( imax-1, A( 1, imax ), 1, W( 1, KW-1 ), 1 )
               W( imax, KW-1 ) = DBLE( A( imax, imax ) )
               CALL ZCOPY( K-imax, A( imax, imax+1 ), LDA,
     $                     W( imax+1, KW-1 ), 1 )
               CALL ZLACGV( K-imax, W( imax+1, KW-1 ), 1 )
               IF( K.LT.N ) THEN
                  CALL ZGEMV( 'No transpose', K, N-K, -CONE,
     $                        A( 1, K+1 ), LDA, W( imax, KW+1 ), LDW,
     $                        CONE, W( 1, KW-1 ), 1 )
                  W( imax, KW-1 ) = DBLE( W( imax, KW-1 ) )
               END IF
*
*              jmax is the column-index of the largest off-diagonal
*              element in row imax, and rowmax is its absolute value
*
               jmax = imax + IZAMAX( K-imax, W( imax+1, KW-1 ), 1 )
               rowmax = CABS1( W( jmax, KW-1 ) )
               IF( imax.GT.1 ) THEN
                  jmax = IZAMAX( imax-1, W( 1, KW-1 ), 1 )
                  rowmax = MAX( rowmax, CABS1( W( jmax, KW-1 ) ) )
               END IF
*
               IF( abs_akk.GE.alpha*colmax*( colmax / rowmax ) ) THEN
*
*                 no interchange, use 1-by-1 pivot block
*
                  kp = K
               ELSE IF( ABS( DBLE( W( imax, KW-1 ) ) ).GE.alpha*rowmax )
     $                   THEN
*
*                 interchange rows and columns K and imax, use 1-by-1
*                 pivot block
*
                  kp = imax
*
*                 copy column KW-1 of W to column KW
*
                  CALL ZCOPY( K, W( 1, KW-1 ), 1, W( 1, KW ), 1 )
               ELSE
*
*                 interchange rows and columns K-1 and imax, use 2-by-2
*                 pivot block
*
                  kp = imax
                  kstep = 2
               END IF
            END IF
*
            kk = K - kstep + 1
            kkW = NB + kk - N
*
*           Updated column kp is already stored in column kkW of W
*
            IF( kp.NE.kk ) THEN
*
*              Copy non-updated column kk to column kp
*
               A( kp, kp ) = DBLE( A( kk, kk ) )
               CALL ZCOPY( kk-1-kp, A( kp+1, kk ), 1, A( kp, kp+1 ),
     $                     LDA )
               CALL ZLACGV( kk-1-kp, A( kp, kp+1 ), LDA )
               CALL ZCOPY( kp-1, A( 1, kk ), 1, A( 1, kp ), 1 )
*
*              Interchange rows kk and kp in last kk columns of A and W
*
               IF( kk.LT.N )
     $            CALL ZSWAP( N-kk, A( kk, kk+1 ), LDA, A( kp, kk+1 ),
     $                        LDA )
               CALL ZSWAP( N-kk+1, W( kk, kkW ), LDW, W( kp, kkW ),
     $                     LDW )
            END IF
*
            IF( kstep.EQ.1 ) THEN
*
*              1-by-1 pivot block D(k): column KW of W now holds
*
*              W(k) = U(k)*D(k)
*
*              where U(k) is the k-th column of U
*
*              Store U(k) in column k of A
*
               CALL ZCOPY( K, W( 1, KW ), 1, A( 1, K ), 1 )
               R1 = ONE / DBLE( A( K, K ) )
               CALL ZDSCAL( K-1, R1, A( 1, K ), 1 )
*
*              Conjugate W(k)
*
               CALL ZLACGV( K-1, W( 1, KW ), 1 )
            ELSE
*
*              2-by-2 pivot block D(k): columns KW and KW-1 of W now
*              hold
*
*              ( W(k-1) W(k) ) = ( U(k-1) U(k) )*D(k)
*
*              where U(k) and U(k-1) are the k-th and (k-1)-th columns
*              of U
*
               IF( K.GT.2 ) THEN
*
*                 Store U(k) and U(k-1) in columns k and k-1 of A
*
                  D21 = W( K-1, KW )
                  D11 = W( K, KW ) / DCONJG( D21 )
                  D22 = W( K-1, KW-1 ) / D21
                  T = ONE / ( DBLE( D11*D22 )-ONE )
                  D21 = T / D21
                  DO 20 J = 1, K - 2
                     A( J, K-1 ) = D21*( D11*W( J, KW-1 )-W( J, KW ) )
                     A( J, K ) = DCONJG( D21 )*
     $                           ( D22*W( J, KW )-W( J, KW-1 ) )
   20             CONTINUE
               END IF
*
*              Copy D(k) to A
*
               A( K-1, K-1 ) = W( K-1, KW-1 )
               A( K-1, K ) = W( K-1, KW )
               A( K, K ) = W( K, KW )
*
*              Conjugate W(k) and W(k-1)
*
               CALL ZLACGV( K-1, W( 1, KW ), 1 )
               CALL ZLACGV( K-2, W( 1, KW-1 ), 1 )
            END IF
         END IF
*
*        Store details of the interchanges in ipiv
*
         IF( kstep.EQ.1 ) THEN
            ipiv( K ) = kp
         ELSE
            ipiv( K ) = -kp
            ipiv( K-1 ) = -kp
         END IF
*
*        Decrease K and return to the start of the main loop
*
         K = K - kstep
         GO TO 10
*
   30    CONTINUE
*
*        Update the upper triangle of A11 (= A(1:k,1:k)) as
*
*        A11 := A11 - U12*D*U12' = A11 - U12*W'
*
*        computing blocks of NB columns at a time (note that conjg(W) is
*        actually stored)
*
         DO 50 J = ( ( K-1 ) / NB )*NB + 1, 1, -NB
            JB = MIN( NB, K-J+1 )
*
*           Update the upper triangle of the diagonal block
*
            DO 40 JJ = J, J + JB - 1
               A( JJ, JJ ) = DBLE( A( JJ, JJ ) )
               CALL ZGEMV( 'No transpose', JJ-J+1, N-K, -CONE,
     $                     A( J, K+1 ), LDA, W( JJ, KW+1 ), LDW, CONE,
     $                     A( J, JJ ), 1 )
               A( JJ, JJ ) = DBLE( A( JJ, JJ ) )
   40       CONTINUE
*
*           Update the rectangular superdiagonal block
*
            CALL ZGEMM( 'No transpose', 'Transpose', J-1, JB, N-K,
     $                  -CONE, A( 1, K+1 ), LDA, W( J, KW+1 ), LDW,
     $                  CONE, A( 1, J ), LDA )
   50    CONTINUE
*
*        Put U12 in standard form by partially undoing the interchanges
*        in columns k+1:n
*
         J = K + 1
   60    CONTINUE
         JJ = J
         JP = ipiv( J )
         IF( JP.LT.0 ) THEN
            JP = -JP
            J = J + 1
         END IF
         J = J + 1
         IF( JP.NE.JJ .AND. J.LE.N )
     $      CALL ZSWAP( N-J+1, A( JP, J ), LDA, A( JJ, J ), LDA )
         IF( J.LE.N )
     $      GO TO 60
*
*        Set KB to the number of columns factorized
*
         KB = N - K
*/
  } else {

     /* Factorize the leading columns of A using the lower triangle
        of A and working forwards, and compute the matrix W = L21*D
        for use in updating A22 (note that conjg(W) is actually stored)

        K is the main loop index, increasing from 1 in steps of 1 or 2 */

     int k;
     for (k = 0; k < min(nb-1,n); k += kstep) {

         /* Copy column K of A to column K of W and update it */

         /* -------------------------------------------------------------- */
         magmablasSetKernelStream( stream[0] );
         trace_gpu_start( 0, 0, "copy", "copyAk" );
         magma_zcopy( n-k, &dA( k, k ), 1, &dW( k, k ), 1 );

         // set imaginary part of diagonal to be zero
         #if defined(PRECISION_z)
         magma_dsetvector_async( 1, &d_zero,1, ((double*)&dW( k, k ))+1,1, stream[0] );
         #elif defined(PRECISION_c)
         magma_ssetvector_async( 1, &d_zero,1, ((double*)&dW( k, k ))+1,1, stream[0] );
         #endif
         trace_gpu_end( 0, 0 );
         /* -------------------------------------------------------------- */

         magmablasSetKernelStream( stream[0] );
         trace_gpu_start( 0, 0, "gemv", "gemv" );
         magma_zgemv( MagmaNoTrans, n-k, k, c_mone, &dA( k, 0 ), ldda, 
                      &dW( k, 0 ), lddw, c_one, &dW( k, k ), ione );
         // re-set imaginary part of diagonal to be zero
         #if defined(PRECISION_z)
         magma_dsetvector_async( 1, &d_zero,1, ((double*)&dW( k, k ))+1,1, stream[0] );
         #elif defined(PRECISION_c)
         magma_ssetvector_async( 1, &d_zero,1, ((double*)&dW( k, k ))+1,1, stream[0] );
         #endif
         trace_gpu_end( 0, 0 );

         kstep = 1;

         /* Determine rows and columns to be interchanged and whether
            a 1-by-1 or 2-by-2 pivot block will be used */

         magma_zgetvector_async( 1, &dW( k, k ), 1, &Z, 1, stream[0] );
         magma_queue_sync( stream[0] );
         abs_akk = fabs( MAGMA_Z_REAL( Z ) );

         /* imax is the row-index of the largest off-diagonal element in
            column K, and colmax is its absolute value */

         if( k < n-1 ) {
             // magmablas is one-base
             magmablasSetKernelStream( stream[0] );
             trace_gpu_start( 0, 0, "max", "max" );
             imax = k + magma_izamax( n-k-1, &dW(k+1,k), 1 );
             trace_gpu_end( 0, 0 );
             magma_zgetvector( 1, &dW( imax, k ), 1, &Z, 1 );
             colmax = MAGMA_Z_ABS1( Z );

         } else {
             colmax = d_zero;
         }

         if ( max( abs_akk, colmax ) == 0.0 ) {

             /* Column K is zero: set INFO and continue */

             if( *info == 0 ) *info = k;
             kp = k;

             // make sure the imaginary part of diagonal is zero
             #if defined(PRECISION_z)
             magma_dsetvector_async( 1, &d_zero,1, ((double*)&dA( k, k ))+1,1, stream[0] );
             #elif defined(PRECISION_c)
             magma_ssetvector_async( 1, &d_zero,1, ((double*)&dA( k, k ))+1,1, stream[0] );
             #endif
         } else {
             if ( abs_akk >= alpha*colmax ) {

                 /* no interchange, use 1-by-1 pivot block */

                 kp = k;
             } else {
                 /* Copy column imax to column K+1 of W and update it */

                 magmablasSetKernelStream( stream[0] );
                 trace_gpu_start( 0, 0, "copy", "copy" );
                 #if defined(PRECISION_z) || defined(PRECISION_c)
                 magmablas_zlacpy_cnjg( imax-k, &dA( imax, k ), ldda, &dW( k, k+1 ), 1 );
                 #else 
                 magma_zcopy( imax-k, &dA( imax, k ), ldda, &dW( k, k+1 ), 1 );
                 #endif 

                 magma_zcopy( n-imax, &dA( imax, imax ), 1, &dW( imax, k+1 ), 1 );
                 #if defined(PRECISION_z)
                 magma_dsetvector_async( 1, &d_zero,1, ((double*)&dW( imax, k+1 ))+1,1, stream[0] );
                 #elif defined(PRECISION_c)
                 magma_ssetvector_async( 1, &d_zero,1, ((double*)&dW( imax, k+1 ))+1,1, stream[0] );
                 #endif
                 trace_gpu_end( 0, 0 );

                 magmablasSetKernelStream( stream[0] );
                 trace_gpu_start( 0, 0, "gemv", "gemv" );
                 magma_zgemv( MagmaNoTrans, n-k, k, c_mone, &dA( k, 0 ), ldda, 
                              &dW( imax, 0 ), lddw, c_one, &dW( k, k+1 ), ione );
                 #if defined(PRECISION_z)
                 magma_dsetvector_async( 1, &d_zero,1, ((double*)&dW( imax, k+1 ))+1,1, stream[0] );
                 #elif defined(PRECISION_c)
                 magma_ssetvector_async( 1, &d_zero,1, ((double*)&dW( imax, k+1 ))+1,1, stream[0] );
                 #endif
                 trace_gpu_end( 0, 0 );

                 magma_zgetvector_async( 1, &dW( imax, k+1 ), 1, &Zimax, 1, stream[0] );

                 /* jmax is the column-index of the largest off-diagonal
                    element in row imax, and rowmax is its absolute value */

                 // magmablas is one-base
                 magmablasSetKernelStream( stream[0] );
                 trace_gpu_start( 0, 0, "max", "max" );
                 jmax = k-1 + magma_izamax( imax-k, &dW(k, k+1), 1 );
                 trace_gpu_end( 0, 0 );
                 magma_zgetvector( 1, &dW( jmax, k+1 ), 1, &Z, 1 );
                 rowmax = MAGMA_Z_ABS1( Z );
                 if( imax < n-1 ) {
                     // magmablas is one-base
                     magmablasSetKernelStream( stream[0] );
                     trace_gpu_start( 0, 0, "max", "max" );
                     jmax = imax + magma_izamax( (n-1)-imax, &dW( imax+1, k+1 ), 1 );
                     trace_gpu_end( 0, 0 );
                     magma_zgetvector( 1, &dW( jmax, k+1 ), 1, &Z, 1 );
                     rowmax = max( rowmax, MAGMA_Z_ABS1( Z ) );
                 }

                 if( abs_akk >= alpha*colmax*( colmax / rowmax ) ) {

                     /* no interchange, use 1-by-1 pivot block */
                     kp = k;
                 } else if( fabs( MAGMA_Z_REAL( Zimax ) ) >= alpha*rowmax ) {

                     /* interchange rows and columns K and imax, use 1-by-1
                        pivot block */
                     kp = imax;

                     /* copy column K+1 of W to column K */
                     magmablasSetKernelStream( stream[0] );
                     trace_gpu_start( 0, 0, "copy", "copy" );
                     magma_zcopy( n-k, &dW( k, k+1 ), 1, &dW( k, k ), 1 );
                     trace_gpu_end( 0, 0 );
                 } else {

                     /* interchange rows and columns K+1 and imax, use 2-by-2
                        pivot block */

                     kp = imax;
                     kstep = 2;
                 }
             }

             kk = k + kstep - 1;

             /* Updated column kp is already stored in column kk of W */

             if( kp != kk ) {

                 /* Copy non-updated column kk to column kp */

                 /* ------------------------------------------------------------------ */
                 magmablasSetKernelStream( stream[0] );
                 trace_gpu_start( 0, 0, "copy", "copy" );
                 #if defined(PRECISION_z) || defined(PRECISION_c)
                 magmablas_zlacpy_cnjg( kp-kk, &dA( kk, kk ), 1, &dA( kp, kk ), ldda );
                 #else
                 magma_zcopy( kp-kk, &dA( kk, kk ), 1, &dA( kp, kk ), ldda );
                 #endif
                 if ( kp < n ) {
                     magma_zcopy( n-kp, &dA( kp, kk), 1, &dA( kp, kp ), 1 );
                 }
                 trace_gpu_end( 0, 0 );
                 /* ------------------------------------------------------------------ */

                 /* Interchange rows kk and kp in first kk columns of A and W */

                 trace_gpu_start( 0, 0, "permute", "swap-backward" );
                 magmablas_zswap( kk+1, &dA( kk, 0 ), ldda, &dA( kp, 0 ), ldda );
                 magmablas_zswap( kk+1, &dW( kk, 0 ), lddw, &dW( kp, 0 ), lddw );
                 trace_gpu_end( 0, 0 );
             }

             if ( kstep == 1 ) {

                 /* 1-by-1 pivot block D(k): column k of W now holds

                    W(k) = L(k)*D(k)

                    where L(k) is the k-th column of L

                    Store L(k) in column k of A */
                 magmablasSetKernelStream( stream[0] );
                 trace_gpu_start( 0, 0, "copy", "copy" );
                 magma_zcopy( n-k, &dW( k, k ), 1, &dA( k, k ), 1 );
                 trace_gpu_end( 0, 0 );

                 if ( k < n-1 ) {
                     magma_zgetvector_async( 1, &dA( k, k ), 1, &Z, 1, stream[0] );
                     R1 = d_one / MAGMA_Z_REAL( Z );
                     magma_queue_sync( stream[0] );
                     trace_gpu_start( 0, 0, "scal", "scal-1" );
                     magma_zdscal((n-1)-k, R1, &dA( k+1 , k ), 1);
                     trace_gpu_end( 0, 0 );

                     /* Conjugate W(k) */
                     #if defined(PRECISION_z) || defined(PRECISION_c)
                     magmablas_zlacpy_cnjg( (n-1)-k, &dW( k+1, k ),1, &dW( k+1, k ),1 );
                     #endif
                 }
             } else {

                 /* 2-by-2 pivot block D(k): columns k and k+1 of W now hold

                 ( W(k) W(k+1) ) = ( L(k) L(k+1) )*D(k)

                 where L(k) and L(k+1) are the k-th and (k+1)-th columns
                 of L */

                 magmablasSetKernelStream( stream[0] );
                 trace_gpu_start( 0, 0, "scal", "scal-2" );
                 magmablas_zlascl_2x2( MagmaFull, MagmaNoTrans, n-(k+2), &dW(k,k), lddw, &dA(k+2,k), ldda, &iinfo );

                 /* Copy D(k) to A */
                 magma_zcopymatrix( 2,2, &dW( k, k ), lddw, &dA( k, k ), ldda );

                 /* Conjugate W(k) and W(k+1) */
                 #if defined(PRECISION_z) || defined(PRECISION_c)
                 magmablas_zlacpy_cnjg( (n-1)-k,   &dW( k+1, k ),1, &dW( k+1, k ),1 );
                 magmablas_zlacpy_cnjg( (n-1)-k-1, &dW( k+2, k+1 ), 1, &dW( k+2, k+1 ), 1 );
                 #endif
                 trace_gpu_end( 0, 0 );
             }
         }

         /* Store details of the interchanges in ipiv */

         if ( kstep == 1 ) {
             ipiv[k] = kp+1;
         } else {
             ipiv[k] = -kp-1;
             ipiv[k+1] = -kp-1;
         }
     } 

     /* Update the lower triangle of A22 (= A(k:n,k:n)) as

        A22 := A22 - L21*D*L21' = A22 - L21*W'

        computing blocks of NB columns at a time (note that conjg(W) is
        actually stored) */

     for( int j = k; j < n; j += nb ) {
         int jb = min( nb, n-j );

         /* Update the lower triangle of the diagonal block */

         #ifdef SYMMETRIC_UPDATE
         for (int jj = j; jj < j + jb; jj++) {
             int jnb = j + jb - jj;

             /* -------------------------------------------------------- */
             magma_zgemv( MagmaNoTrans, jnb, k, c_mone, &dA( jj, 0 ), ldda, 
                          &dW( jj, 0 ), lddw, c_one, &dA( jj, jj ), ione );
             /* -------------------------------------------------------- */
         }

         /* Update the rectangular subdiagonal block */

         if( j+jb < n ) {
             int nk = n - (j+jb);

             /* -------------------------------------------- */
             magmablasSetKernelStream( stream[0] );
             magma_zgemm( MagmaNoTrans, MagmaTrans, nk, jb, k, 
                          c_mone, &dA( j+jb, 0 ), ldda, 
                                  &dW( j, 0 ),    lddw,
                          c_one,  &dA( j+jb, j ), ldda );
             /* ------------------------------------------- */
         }
         #else
         trace_gpu_start( 0, 0, "gemm", "gemm" );
         magmablasSetKernelStream( stream[0] );
         #if defined(PRECISION_z)
         //for (int jj=0; jj<jb; jj++) magma_dsetvector_async( 1, &d_zero,1, ((double*)&dA( j+jj, j+jj ))+1,1, stream[0] );
         magmablas_dlaset(MagmaUpperLower, 1,jb, d_zero,d_zero, ((double*)&dA( j, j ))+1, 2*(1+ldda) );
         #elif defined(PRECISION_c)
         //for (int jj=0; jj<jb; jj++) magma_ssetvector_async( 1, &d_zero,1, ((double*)&dA( j+jj, j+jj ))+1,1, stream[0] );
         magmablas_slaset(MagmaUpperLower, 1,jb, d_zero,d_zero, ((double*)&dA( j, j ))+1, 2*(1+ldda) );
         #endif
         magma_zgemm( MagmaNoTrans, MagmaTrans, n-j, jb, k, 
                      c_mone, &dA( j, 0 ), ldda, 
                              &dW( j, 0 ), lddw,
                      c_one,  &dA( j, j ), ldda );
         #if defined(PRECISION_z)
         //for (int jj=0; jj<jb; jj++) magma_dsetvector_async( 1, &d_zero,1, ((double*)&dA( j+jj, j+jj ))+1,1, stream[0] );
         magmablas_dlaset(MagmaUpperLower, 1,jb, d_zero,d_zero, ((double*)&dA( j, j ))+1, 2*(1+ldda) );
         #elif defined(PRECISION_c)
         //for (int jj=0; jj<jb; jj++) magma_ssetvector_async( 1, &d_zero,1, ((double*)&dA( j+jj, j+jj ))+1,1, stream[0] );
         magmablas_slaset(MagmaUpperLower, 1,jb, d_zero,d_zero, ((double*)&dA( j, j ))+1, 2*(1+ldda) );
         #endif
         trace_gpu_end( 0, 0 );
         #endif
     }

     /* Put L21 in standard form by partially undoing the interchanges
        in columns 1:k-1 */

     for (int j = k; j > 0;) {
         int jj = j;
         int jp = ipiv[j-1];
         if( jp < 0 ) {
             jp = -jp;
             j--;
         }
         j--;
         if ( jp != jj && j >= 1 ) {
             magmablasSetKernelStream( stream[0] );
             trace_gpu_start( 0, 0, "permute", "perm" );
             magmablas_zswap( j, &dA( jp-1, 0 ), ldda, &dA( jj-1, 0 ), ldda );
             trace_gpu_end( 0, 0 );
             magma_queue_sync( stream[0] ); 
         }
     }
     // copying the panel back to CPU
     magma_event_record( event[0], stream[0] );
     magma_queue_wait_event( stream[1], event[0] ); 
     trace_gpu_start( 0, 1, "get", "get" );
     magma_zgetmatrix_async( n,k, &dA(0,0),ldda, &A(0,0),lda, stream[1] );
     trace_gpu_end( 0, 1 );
     /* Set KB to the number of columns factorized */
     *kb = k;
  }

  return *info;
  /* End of ZLAHEF */
}
