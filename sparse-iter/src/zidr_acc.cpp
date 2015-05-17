/*
    -- MAGMA (version 1.1) --
       Univ. of Tennessee, Knoxville
       Univ. of California, Berkeley
       Univ. of Colorado, Denver
       @date

       @author Hartwig Anzt
       @author Eduardo Ponce

       @precisions normal z -> s d c
*/

#include "common_magmasparse.h"
#include <cuda_profiler_api.h>
#include <assert.h>

#define RTOLERANCE     lapackf77_dlamch( "E" )
#define ATOLERANCE     lapackf77_dlamch( "E" )


/**
    Purpose
    -------

    Solves a system of linear equations
       A * X = B
    where A is a complex Hermitian N-by-N positive definite matrix A.
    This is a GPU implementation of the Induced Dimension Reduction method.

    Arguments
    ---------

    @param[in]
    A           magma_z_matrix
                input matrix A

    @param[in]
    b           magma_z_matrix
                RHS b

    @param[in,out]
    x           magma_z_matrix*
                solution approximation

    @param[in,out]
    solver_par  magma_z_solver_par*
                solver parameters
    @param[in]
    queue       magma_queue_t
                Queue to execute in.

    @ingroup magmasparse_zposv
    ********************************************************************/

#define MYDEBUG 0
#define WRITEP 0

#if MYDEBUG == 1
#define printD(...) printf(__VA_ARGS__)
#define printMatrix(s,m)
#elif MYDEBUG == 2
#define printD(...) printf(__VA_ARGS__)
#define printMatrix(s,m) magma_zmatrixInfo_acc(s,m)
#else
#define printD(...)
#define printMatrix(s,m)
#endif

#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#define MAX(a,b) (((a) > (b)) ? (a) : (b))

// Notes:
// 2. Overlap kernels using cuBLAS streams
//
// 3. Build dependency graph of IDR(s)-biortho
//
// 4. Apparently, some precision is lost using MAGMA when compared to MATLAB, probably is that matrices are not displayed with full precision on screen.
//
// 5. Optimize: merge kernels, reuse arrays, concurrent kernels


extern "C" void
magma_zmatrixInfo_acc(
    const char *s,
    magma_z_matrix A ) {

    printf(" %s dims = %d x %d\n", s, A.num_rows, A.num_cols);
    printf(" %s location = %d = %s\n", s, A.memory_location, (A.memory_location == Magma_CPU) ? "CPU" : "DEV");
    printf(" %s storage = %d = %s\n", s, A.storage_type, (A.storage_type == Magma_CSR) ? "CSR" : "DENSE");
    printf(" %s major = %d = %s\n", s, A.major, (A.major == MagmaRowMajor) ? "row" : "column");
    printf(" %s nnz = %d\n", s, A.nnz);
    if (A.memory_location == Magma_DEV)
        magma_zprint_gpu( A.num_rows, A.num_cols, A.dval, A.num_rows );
    else
        magma_zprint( A.num_rows, A.num_cols, A.val, A.num_rows );
}


extern "C" magma_int_t
magma_zidr_acc(
    magma_z_matrix A, magma_z_matrix b, magma_z_matrix *x,
    magma_z_solver_par *solver_par,
    magma_queue_t queue )
{
    // set queue for old dense routines
    magma_queue_t orig_queue = NULL;
    magmablasGetKernelStream( &orig_queue );

    // prepare solver feedback
    solver_par->solver = Magma_IDR;
    solver_par->numiter = 0;
    solver_par->info = MAGMA_SUCCESS;

    // local constants
    const magmaDoubleComplex c_zero = MAGMA_Z_ZERO;
    const magmaDoubleComplex c_one = MAGMA_Z_ONE;
    const magmaDoubleComplex c_n_one = MAGMA_Z_NEG_ONE;

    // local variables
    magma_int_t info = 0;
    magma_int_t iseed[4] = { 0, 0, 0, 1 };
    magma_int_t dofb = b.num_rows * b.num_cols;
    magma_int_t inc = 1;
    magma_int_t s;
    magma_int_t dof;
    magma_int_t distr;
    magma_int_t k, i, sk;
    magma_int_t *piv = NULL;
    magma_int_t innerflag;
    double residual;
    double nrm;
    double nrmb;
    double nrmr;
    double nrmt;
    double rho;
    double tolb;
    double angle;
    magmaDoubleComplex om;
    magmaDoubleComplex tr;
    magmaDoubleComplex alpha;
    magmaDoubleComplex beta;
    magmaDoubleComplex mkk;
    magmaDoubleComplex fk;

    // local matrices and vectors
    magma_z_matrix P1 = {Magma_CSR}, dP1 = {Magma_CSR}, dP = {Magma_CSR};
    magma_z_matrix dr = {Magma_CSR};
    magma_z_matrix dG = {Magma_CSR};
    magma_z_matrix dU = {Magma_CSR};
    magma_z_matrix dM1 = {Magma_CSR}, dM = {Magma_CSR};
    magma_z_matrix df = {Magma_CSR};
    magma_z_matrix dt = {Magma_CSR};
    magma_z_matrix dc = {Magma_CSR};
    magma_z_matrix dv1 = {Magma_CSR}, dv = {Magma_CSR};

    // arrays for scalar products
    magma_z_matrix dskp = {Magma_CSR}, skp = {Magma_CSR};
    
    //workspace for merged dot product
    magmaDoubleComplex *d1=NULL, *d2=NULL;
    d1 = NULL;
    d2 = NULL;

    
    // local performance variables
    long long int gpumem = 0;

    // chronometry
    real_Double_t tempo1, tempo2;

    gpumem += (A.nnz * sizeof(magmaDoubleComplex)) + (A.nnz * sizeof(magma_index_t)) + ((A.num_rows + 1) * sizeof(magma_index_t));

    // check if matrix A is square
    if ( A.num_rows != A.num_cols ) {
        printD("Error! matrix must be square.\n");
        info = MAGMA_ERR;
        goto cleanup;
    }

    // initial s space
    // hack --> use "--restart" option as the shadow space number
    s = 1;
    if ( solver_par->restart != 30 ) {
        if ( solver_par->restart > A.num_cols )
            s = A.num_cols;
        else
            s = solver_par->restart;
    }
    solver_par->restart = s;

    // set max iterations
    solver_par->maxiter = MIN(2 * A.num_cols, solver_par->maxiter);

    // initial angle
    angle = 0.7;

    // initial solution vector
    // x = 0
    //magmablas_zlaset( MagmaFull, x->num_rows, x->num_cols, c_zero, c_zero, x->dval, x->num_rows );
    printMatrix("X", *x);
    gpumem += x->nnz * sizeof(magmaDoubleComplex);

    // initial RHS
    // b = 1
    //magmablas_zlaset( MagmaFull, b.num_rows, b.num_cols, c_one, c_one, b.dval, b.num_rows );
    printMatrix("B", b);
    gpumem += b.nnz * sizeof(magmaDoubleComplex);

    // |b|
    nrmb = magma_dznrm2( b.num_rows, b.dval, inc );

    // check for |b| == 0
    printD("init norm(b) ..........%f\n", nrmb);
    if ( nrmb == 0.0 ) {
        printD("RHS is zero, exiting...\n");
        magma_zscal( x->num_rows*x->num_cols, MAGMA_Z_ZERO, x->dval, inc );
        solver_par->init_res = 0.0;
        solver_par->final_res = 0.0;
        solver_par->iter_res = 0.0;
        solver_par->runtime = 0.0;
        goto cleanup;
    }

    // P = randn(n, s)
    // P = ortho(P)
//---------------------------------------
    // P1 = 0.0
    CHECK( magma_zvinit( &P1, Magma_CPU, A.num_cols, s, c_zero, queue ));

    // P1 = randn(n, s)
    distr = 3;        // 1 = unif (0,1), 2 = unif (-1,1), 3 = normal (0,1) 
    dof = P1.num_rows * P1.num_cols;
    lapackf77_zlarnv( &distr, iseed, &dof, P1.val );
    printMatrix("P1", P1);

    // transfer P1 to device
    CHECK( magma_zmtransfer( P1, &dP1, Magma_CPU, Magma_DEV, queue ));
    magma_zmfree( &P1, queue );

    // P = ortho(P1)
    if ( dP1.num_cols > 1 ) {
        // P = magma_zqr(P1), QR factorization
        CHECK( magma_zqr( dP1.num_rows, dP1.num_cols, dP1, &dP, NULL, queue ) );
    } else {
        // P = P1 / |P1|
        dof = dP1.num_rows * dP1.num_cols;        // can remove
        nrm = magma_dznrm2( dof, dP1.dval, inc );
        nrm = 1.0 / nrm;
        magma_zdscal( dof, nrm, dP1.dval, inc );
        CHECK( magma_zmtransfer( dP1, &dP, Magma_DEV, Magma_DEV, queue ));
    }
    magma_zmfree(&dP1, queue );
//---------------------------------------
    printMatrix("P", dP);
    gpumem += dP.nnz * sizeof(magmaDoubleComplex);

#if WRITEP == 1
    // Note: write P matrix to file to use in MATLAB for validation
    magma_zprint_gpu( dP.num_rows, dP.num_cols, dP.dval, dP.num_rows );
#endif

    // t = 0
    // make t twice as large to contain both, dt and dr
    CHECK( magma_zvinit( &dt, Magma_DEV, A.num_rows, 2*b.num_cols, c_zero, queue ));
    dt.num_cols = b.num_cols;
    gpumem += dt.nnz * sizeof(magmaDoubleComplex);

    // initial residual
    // r = b - A x
    CHECK( magma_zvinit( &dr, Magma_DEV, b.num_rows, b.num_cols, c_zero, queue ));
    // redirect the dr.dval to the second part of dt
    magma_free( dr.dval );
    dr.dval = dt.dval+b.num_rows*b.num_cols;
    CHECK(  magma_zresidualvec( A, b, *x, &dr, &nrmr, queue));
    
    // allocate memory for the scalar products
    CHECK( magma_zvinit( &dskp, Magma_DEV, 2, 1, c_zero, queue ));
    gpumem += dskp.nnz * sizeof(magmaDoubleComplex);
    CHECK( magma_zvinit( &skp, Magma_CPU, 2, 1, c_zero, queue ));
    
    // workspace for merged dot product
    CHECK( magma_zmalloc( &d1, b.num_rows*b.num_cols*(2) ));
    CHECK( magma_zmalloc( &d2, b.num_rows*b.num_cols*(2) ));
    
    
    printMatrix("R" , dr);
    gpumem += dr.nnz * sizeof(magmaDoubleComplex);

    // |r|
    solver_par->init_res = nrmr;
    if ( solver_par->verbose > 0 ) {
        solver_par->res_vec[0] = (real_Double_t)nrmr;
    }
   
    // relative tolerance
    // Note: shouldn't it be nrmb instead of nrmr?
    tolb = nrmb * solver_par->epsilon;
    //tolb = nrmr * solver_par->epsilon;
    if ( tolb < ATOLERANCE ) {
        tolb = ATOLERANCE;
    }
    // check if initial is guess good enough
    if ( nrmr <= tolb ) {
        solver_par->final_res = solver_par->init_res;
        solver_par->iter_res = solver_par->init_res;
        goto cleanup;
    }

    // G(n,s) = 0
    CHECK( magma_zvinit( &dG, Magma_DEV, A.num_cols, s, c_zero, queue ));
    gpumem += dG.nnz * sizeof(magmaDoubleComplex);

    // U(n,s) = 0
    CHECK( magma_zvinit( &dU, Magma_DEV, A.num_cols, s, c_zero, queue ));
    gpumem += dU.nnz * sizeof(magmaDoubleComplex);

    // M1 = 0
    // M(s,s) = I
    CHECK( magma_zvinit( &dM1, Magma_DEV, s, s, c_zero, queue ));
    CHECK( magma_zvinit( &dM, Magma_DEV, s, s, c_zero, queue ));
    magmablas_zlaset( MagmaFull, s, s, c_zero, c_one, dM.dval, s );
    gpumem += 2 * dM.nnz * sizeof(magmaDoubleComplex);

    // f = 0
    CHECK( magma_zvinit( &df, Magma_DEV, dP.num_cols, dr.num_cols, c_zero, queue ));
    gpumem += df.nnz * sizeof(magmaDoubleComplex);

    // c = 0
    CHECK( magma_zvinit( &dc, Magma_DEV, dM.num_cols, df.num_cols, c_zero, queue ));
    gpumem += dc.nnz * sizeof(magmaDoubleComplex);

    // v1 = 0
    // v = 0
    CHECK( magma_zvinit( &dv1, Magma_DEV, dr.num_rows, dr.num_cols, c_zero, queue ));
    CHECK( magma_zvinit( &dv, Magma_DEV, dr.num_rows, dr.num_cols, c_zero, queue ));
    gpumem += 2 * dv.nnz * sizeof(magmaDoubleComplex);

    // piv = 0
    CHECK( magma_imalloc_pinned(&piv, s));

    // om = 1
    om = MAGMA_Z_ONE;

    
    //--------------START TIME---------------
    // chronometry
    tempo1 = magma_sync_wtime( queue );
    if ( solver_par->verbose > 0 ) {
        solver_par->timing[0] = 0.0;
    }
    
    innerflag = 0;
    solver_par->numiter = 0;
cudaProfilerStart();
    // start iteration
    do
    {
        solver_par->numiter++;
    
        // new RHS for small systems
        // f = (r' P)' = P' r
        magmablas_zgemv( MagmaConjTrans, dP.num_rows, dP.num_cols, c_one, dP.dval, dP.num_rows, dr.dval, inc, c_zero, df.dval, inc );
        printMatrix("F", df);

        // shadow space loop
        for ( k = 0; k < s; ++k ) {
            sk = s - k;
    
            // solve small system and make v orthogonal to P
            // f(k:s) = M(k:s,k:s) c(k:s)
//---------------------------------------
            // c(k:s) = f(k:s)
            magma_zcopy( sk, &df.dval[k], inc, &dc.dval[k], inc );

            // M1 = M
            magma_zcopy( dM.num_rows * dM.num_cols, dM.dval, inc, dM1.dval, inc );

            // c(k:s) = M1(k:s,k:s) \ c(k:s)
            CHECK( magma_zgesv_gpu( sk, dc.num_cols, &dM1.dval[k*dM1.num_rows+k], dM1.num_rows, piv, &dc.dval[k], dc.num_rows, &info ) );
//---------------------------------------
            printMatrix("C", dc);

            // v1 = r - G(:,k:s) c(k:s)
//---------------------------------------
            // v1 = r
            magma_zcopy( dr.num_rows * dr.num_cols, dr.dval, inc, dv1.dval, inc );

            // v1 = v1 - G(:,k:s) c(k:s)
            magmablas_zgemv( MagmaNoTrans, dG.num_rows, sk, c_n_one, &dG.dval[k*dG.num_rows], dG.num_rows, &dc.dval[k], inc, c_one, dv1.dval, inc );
//---------------------------------------
            printMatrix("V1", dv1);

            // compute new U
            // U(:,k) = om * v1 + U(:,k:s) c(k:s)
//---------------------------------------
            // v1 = om * v1 + U(:,k:s) c(k:s)
            magmablas_zgemv( MagmaNoTrans, dU.num_rows, sk, c_one, &dU.dval[k*dU.num_rows], dU.num_rows, &dc.dval[k], inc, om, dv1.dval, inc );

            // U(:,k) = v1
            magma_zcopy( dU.num_rows, dv1.dval, inc, &dU.dval[k*dU.num_rows], inc );
//---------------------------------------
            printMatrix("U", dU);

            // compute new U and G
            // G(:,k) = A U(:,k)
//---------------------------------------
            // v = A v1
            CHECK( magma_z_spmv( c_one, A, dv1, c_zero, dv, queue ));

            // G(:,k) = v
            magma_zcopy( dG.num_rows, dv.dval, inc, &dG.dval[k*dG.num_rows], inc );
//---------------------------------------
            printMatrix("G", dG);


            // bi-orthogonalize the new basis vectors
            for ( i = 0; i < k; ++i ) {

                // alpha = P(:,i)' G(:,k) / M(i,i)
//---------------------------------------
                // alpha = P(:,i)' G(:,k)
                alpha = magma_zdotc( dP.num_rows, &dP.dval[i*dP.num_rows], inc, &dG.dval[k*dG.num_rows], inc );
                
                // alpha = alpha / M(i,i)
                magma_zgetvector( 1, &dM.dval[i*dM.num_rows+i], inc, &mkk, inc );
                alpha = alpha / mkk;
//---------------------------------------
                printD("bi-ortho: i, k, alpha ...................%d, %d, (%f, %f)\n", i, k, MAGMA_Z_REAL(alpha), MAGMA_Z_IMAG(alpha));

                // G(:,k) = G(:,k) - alpha * G(:,i)
                magma_zaxpy( dG.num_rows, -alpha, &dG.dval[i*dG.num_rows], inc, &dG.dval[k*dG.num_rows], inc );
                printMatrix("G", dG);

                // U(:,k) = U(:,k) - alpha * U(:,i)
                magma_zaxpy( dU.num_rows, -alpha, &dU.dval[i*dU.num_rows], inc, &dU.dval[k*dU.num_rows], inc );
                printMatrix("U", dU);
            }

            // new column of M = P'G, first k-1 entries are zero
            // Note: need to verify that first k-1 entries are zero

            // M(k:s,k) = (G(:,k)' P(:,k:s))' = P(:,k:s)' G(:,k)
            magmablas_zgemv( MagmaConjTrans, dP.num_rows, sk, c_one, &dP.dval[k*dP.num_rows], dP.num_rows, &dG.dval[k*dG.num_rows], inc, c_zero, &dM.dval[k*dM.num_rows+k], inc );
            printMatrix("M", dM);

            // check M(k,k) == 0
            magma_zgetvector( 1, &dM.dval[k*dM.num_rows+k], inc, &mkk, inc );
            if ( MAGMA_Z_EQUAL(mkk, MAGMA_Z_ZERO) ) {
                info = MAGMA_DIVERGENCE;
                innerflag = 1;
                break;
            }

            // beta = f(k) / M(k,k)
            magma_zgetvector( 1, &df.dval[k], inc, &fk, inc );
            beta = fk / mkk;
            printD("beta: k ...................%d, (%f, %f)\n", k, MAGMA_Z_REAL(beta), MAGMA_Z_IMAG(beta));

            // x = x + beta * U(:,k)
            magma_zaxpy( x->num_rows, beta, &dU.dval[k*dU.num_rows], inc, x->dval, inc );
            printMatrix("X", *x);

            // make r orthogonal to q_i, i = 1..k
            // r = r - beta * G(:,k)
            magma_zaxpy( dr.num_rows, -beta, &dG.dval[k*dG.num_rows], inc, dr.dval, inc );
            printMatrix("R", dr);

            // |r|
            nrmr = magma_dznrm2( dofb, dr.dval, inc );
            printD("norm(r): k ...................%d, %f\n", k, nrmr);

            // store current timing and residual
            if ( solver_par->verbose > 0 ) {
                tempo2 = magma_sync_wtime( queue );
                if ( (solver_par->numiter)%solver_par->verbose==0 ) {
                    solver_par->res_vec[(solver_par->numiter)/solver_par->verbose]
                            = (real_Double_t) nrmr;
                    solver_par->timing[(solver_par->numiter)/solver_par->verbose]
                            = (real_Double_t) tempo2-tempo1;
                }
            }

            // check convergence or iteration limit
            if ( nrmr <= tolb || solver_par->numiter >= solver_par->maxiter ) {
                innerflag = 1;
                break;
            }

            // new f = P' r (first k components are zero)
            if ( (k + 1) < s ) {
                // f(k+1:s) = f(k+1:s) - beta * M(k+1:s,k)
                magma_zaxpy( sk - 1, -beta, &dM.dval[k*dM.num_rows+(k+1)], inc, &df.dval[k+1], inc );
                printMatrix("F", df);
            }

            // iter = iter + 1
            solver_par->numiter++;
        }

        // check convergence or iteration limit or invalid of inner loop
        if ( innerflag == 1 ) {
            break;
        }

        // v = r
        magma_zcopy( dr.num_rows * dr.num_cols, dr.dval, inc, dv.dval, inc );
        printMatrix("V", dv);

        // t = A v
        CHECK( magma_z_spmv( c_one, A, dv, c_zero, dt, queue ));
        printMatrix("T", dt);

        // computation of a new omega
        // om = omega(t, r, angle);
//---------------------------------------
        /*
        // |t|
        dof = dt.num_rows * dt.num_cols;
        nrmt = magma_dznrm2( dof, dt.dval, inc );

        // tr = t' r
        tr = magma_zdotc( dr.num_rows, dt.dval, inc, dr.dval, inc );

        // rho = abs(tr / (|t| * |r|))
        rho = fabs( MAGMA_Z_REAL(tr) / (nrmt * nrmr) );

        // om = tr / (|t| * |t|)
        om = tr / (nrmt * nrmt);
        if ( rho < angle )
            om = om * angle / rho;
        */
//---------------------------------------
// replace by merged dot product
//---------------------------------------
        // |t|
        dof = dt.num_rows * dt.num_cols;
        CHECK( magma_zmdotc( dof, 2, dt.dval, dt.dval, d1, d2, dskp.dval, queue ));
        magma_zgetvector( 2 , dskp.dval, 1, skp.val, 1 );
        
        nrmt = sqrt(MAGMA_Z_REAL(skp.val[0]));
        tr = skp.val[1];
        
        // rho = abs(tr / (|t| * |r|))
        rho = fabs( MAGMA_Z_REAL(tr) / (nrmt * nrmr) );

        // om = tr / (|t| * |t|)
        om = tr / (nrmt * nrmt);
        if ( rho < angle )
            om = om * angle / rho;
//---------------------------------------



        printD("omega: k .................... %d, (%f, %f)\n", k, MAGMA_Z_REAL(om), MAGMA_Z_IMAG(om));
        if ( MAGMA_Z_EQUAL(om, MAGMA_Z_ZERO) ) {
            info = MAGMA_DIVERGENCE;
            break;
        }

        // update approximation vector
        // x = x + om * v
        magma_zaxpy(x->num_rows, om, dv.dval, inc, x->dval, inc);
        printMatrix("X", *x);

        // update residual vector
        // r = r - om * t
        magma_zaxpy(dr.num_rows, -om, dt.dval, inc, dr.dval, inc);
        printMatrix("R", dr);

        // residual norm
        nrmr = magma_dznrm2( dofb, dr.dval, inc );
        printD("norm(r): k ...................%d, %f\n", k, nrmr);

        // store current timing and residual
        if ( solver_par->verbose > 0 ) {
            tempo2 = magma_sync_wtime( queue );
            if ( (solver_par->numiter)%solver_par->verbose==0 ) {
                solver_par->res_vec[(solver_par->numiter)/solver_par->verbose]
                        = (real_Double_t) nrmr;
                solver_par->timing[(solver_par->numiter)/solver_par->verbose]
                        = (real_Double_t) tempo2-tempo1;
            }
        }

        // check convergence or iteration limit
        if ( nrmr <= tolb || solver_par->numiter >= solver_par->maxiter ) {
            break;
        }

#if MYDEBUG == 2
        // Note: exit loop after a few iterations
        if ( solver_par->numiter + 1 >= (2 * (s + 1)) ) {
            break;
        }
#endif
    }
    while ( solver_par->numiter + 1 <= solver_par->maxiter );
cudaProfilerStop();
    // get last iteration timing
    tempo2 = magma_sync_wtime( queue );
    solver_par->runtime = (real_Double_t) tempo2-tempo1;
//--------------STOP TIME----------------

    // last stats
    solver_par->iter_res = nrmr;
    CHECK( magma_zresidualvec( A, b, *x, &dr, &residual, queue ));
    solver_par->final_res = residual;

    // set solver conclusion
    if ( info != MAGMA_SUCCESS ) {
        if ( solver_par->init_res > solver_par->final_res ) {
            info = MAGMA_SLOW_CONVERGENCE;
        }
        else {
            info = MAGMA_DIVERGENCE;
        }
    }
//---------------------------------------

#if MYDEBUG > 0 || WRITEP == 1
    // print local stats
    printf("GPU memory = %f MB\n", (real_Double_t)gpumem / (1<<20));
#endif
    
    
cleanup:
    // free resources
    magma_zmfree(&P1, queue);
    magma_zmfree(&dP1, queue);
    magma_zmfree(&dP, queue);
    //magma_zmfree(&dr, queue);
    magma_zmfree(&dG, queue);
    magma_zmfree(&dU, queue);
    magma_zmfree(&dM1, queue);
    magma_zmfree(&dM, queue);
    magma_zmfree(&df, queue);
    magma_zmfree(&dt, queue);
    magma_zmfree(&dc, queue);
    magma_zmfree(&dv1, queue);
    magma_zmfree(&dv, queue);
    magma_zmfree(&dskp, queue);
    magma_zmfree(&skp, queue);
    magma_free( d1 );
    magma_free( d2 );
    magma_free_pinned(piv);

    magmablasSetKernelStream( orig_queue );
    solver_par->info = info;
    return info;
    /* magma_zidr */
}
