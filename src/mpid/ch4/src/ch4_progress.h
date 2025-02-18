/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#ifndef CH4_PROGRESS_H_INCLUDED
#define CH4_PROGRESS_H_INCLUDED

#include "ch4_impl.h"

/* Global progress (polling every vci) is required for correctness. Currently we adopt the
 * simple approach to do global progress every MPIDI_CH4_PROG_POLL_MASK.
 *
 * TODO: every time we do global progress, there will be a performance lag. We could --
 * * amortize the cost by rotating the global vci to be polled (might be insufficient)
 * * accept user hints (require new user interface)
 */
#define MPIDI_CH4_PROG_POLL_MASK 0xff

#ifdef MPL_TLS
extern MPL_TLS int global_vci_poll_count;
#elif defined(MPL_COMPILER_TLS)
/*
 * If MPL_COMPILER_TLS is defined, use MPL_COMPILER_TLS.  This is the case of Argobots, for example.
 * Argobots does not have MPL_TLS since MPL_COMPILER_TLS (__thread) is not Argobots ULT local.
 * MPL_COMPILER_TLS is executions-stream-local storage from the Argobots viewpoint, but this works
 * well for this progress counter since this global_vci_poll_count is "thread-local" just to avoid
 * any conflict among parallel execution entities (i.e., POSIX threads).
 */
extern MPL_COMPILER_TLS int global_vci_poll_count;
#else
extern int global_vci_poll_count;
#endif

MPL_STATIC_INLINE_PREFIX int MPIDI_do_global_progress(void)
{
    if (MPIDI_global.n_vcis == 1) {
        return 0;
    } else {
        global_vci_poll_count++;
        return ((global_vci_poll_count & MPIDI_CH4_PROG_POLL_MASK) == 0);
    }
}

/* inside per-vci progress */
MPL_STATIC_INLINE_PREFIX void MPIDI_check_progress_made_idx(MPID_Progress_state * state, int idx)
{
    int cur_count = MPL_atomic_relaxed_load_int(&MPIDI_VCI(state->vci[idx]).progress_count);
    if (state->progress_counts[idx] != cur_count) {
        state->progress_counts[idx] = cur_count;
        state->progress_made = 1;
    }
}

/* inside global progress */
MPL_STATIC_INLINE_PREFIX void MPIDI_check_progress_made_vci(MPID_Progress_state * state, int vci)
{
    for (int i = 0; i < state->vci_count; i++) {
        if (vci == state->vci[i]) {
            int cur_count = MPL_atomic_relaxed_load_int(&MPIDI_VCI(state->vci[i]).progress_count);
            if (state->progress_counts[i] != cur_count) {
                state->progress_counts[i] = cur_count;
                state->progress_made = 1;
            }
            break;
        }
    }
}

/* define MPIDI_PROGRESS to make the code more readable (to avoid nested '#ifdef's) */
#ifdef MPIDI_CH4_DIRECT_NETMOD
#define MPIDI_PROGRESS(vci) \
    do { \
        if (state->flag & MPIDI_PROGRESS_NM) { \
            mpi_errno = MPIDI_NM_progress(vci, 0); \
        } \
    } while (0)

#else
#define MPIDI_PROGRESS(vci) \
    do { \
        if (state->flag & MPIDI_PROGRESS_NM) { \
            mpi_errno = MPIDI_NM_progress(vci, 0); \
        } \
        if (state->flag & MPIDI_PROGRESS_SHM && mpi_errno == MPI_SUCCESS) { \
            mpi_errno = MPIDI_SHM_progress(vci, 0); \
        } \
    } while (0)
#endif

MPL_STATIC_INLINE_PREFIX int MPIDI_progress_test(MPID_Progress_state * state, int wait)
{
    int mpi_errno, made_progress;
    mpi_errno = MPI_SUCCESS;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_PROGRESS_TEST);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_PROGRESS_TEST);

#ifdef HAVE_SIGNAL
    if (MPIDI_global.sigusr1_count > MPIDI_global.my_sigusr1_count) {
        MPIDI_global.my_sigusr1_count = MPIDI_global.sigusr1_count;
        mpi_errno = MPIDI_check_for_failed_procs();
        MPIR_ERR_CHECK(mpi_errno);
    }
#endif

    if (state->flag & MPIDI_PROGRESS_HOOKS) {
        mpi_errno = MPIR_Progress_hook_exec_all(&made_progress);
        MPIR_ERR_CHECK(mpi_errno);
    }
    /* todo: progress unexp_list */

#ifdef MPIDI_CH4_USE_WORK_QUEUES
    mpi_errno = MPIDI_workq_vci_progress();
    MPIR_ERR_CHECK(mpi_errno);
#endif

#if MPIDI_CH4_MAX_VCIS == 1
    /* fast path for single vci */
    MPID_THREAD_CS_ENTER(VCI, MPIDI_VCI(0).lock);
    MPIDI_PROGRESS(0);
    if (wait) {
        MPIDI_check_progress_made_idx(state, 0);
    }
    MPID_THREAD_CS_EXIT(VCI, MPIDI_VCI(0).lock);

#else
    /* multiple vci */
    if (MPIDI_do_global_progress()) {
        for (int vci = 0; vci < MPIDI_global.n_vcis; vci++) {
            MPID_THREAD_CS_ENTER(VCI, MPIDI_VCI(vci).lock);
            MPIDI_PROGRESS(vci);
            if (wait) {
                MPIDI_check_progress_made_vci(state, vci);
            }
            MPID_THREAD_CS_EXIT(VCI, MPIDI_VCI(vci).lock);
            MPIR_ERR_CHECK(mpi_errno);
            if (wait && state->progress_made) {
                break;
            }
        }
    } else {
        for (int i = 0; i < state->vci_count; i++) {
            int vci = state->vci[i];
            MPID_THREAD_CS_ENTER(VCI, MPIDI_VCI(vci).lock);
            MPIDI_PROGRESS(vci);
            if (wait) {
                MPIDI_check_progress_made_idx(state, i);
            }
            MPID_THREAD_CS_EXIT(VCI, MPIDI_VCI(vci).lock);
            MPIR_ERR_CHECK(mpi_errno);
            if (wait && state->progress_made) {
                break;
            }
        }
    }
#endif

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_PROGRESS_TEST);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

/* Init with all VCIs. Performance critical path should always pass in explicit
 * state, thus avoid poking all progresses */
MPL_STATIC_INLINE_PREFIX void MPIDI_progress_state_init(MPID_Progress_state * state)
{
    state->flag = MPIDI_PROGRESS_ALL;
    state->progress_made = 0;
    /* global progress by default */
    for (int i = 0; i < MPIDI_global.n_vcis; i++) {
        state->vci[i] = i;
    }
    state->vci_count = MPIDI_global.n_vcis;
}

/* only wait functions need check progress_counts */
MPL_STATIC_INLINE_PREFIX void MPIDI_progress_state_init_count(MPID_Progress_state * state)
{
    /* Note: ugly code to avoid warning -Wmaybe-uninitialized */
#if MPIDI_CH4_MAX_VCIS == 1
    state->progress_counts[0] = MPL_atomic_relaxed_load_int(&MPIDI_VCI(0).progress_count);
#else
    for (int i = 0; i < MPIDI_global.n_vcis; i++) {
        state->progress_counts[i] = MPL_atomic_relaxed_load_int(&MPIDI_VCI(i).progress_count);
    }
#endif
}

MPL_STATIC_INLINE_PREFIX int MPIDI_Progress_test(int flags)
{
    MPID_Progress_state state;
    MPIDI_progress_state_init(&state);
    state.flag = flags;
    return MPIDI_progress_test(&state, 0);
}

/* provide an internal direct progress function. This is used in e.g. RMA, where
 * we need poke internal progress from inside a per-vci lock.
 */
MPL_STATIC_INLINE_PREFIX int MPIDI_progress_test_vci(int vci)
{
    int mpi_errno = MPI_SUCCESS;

    if (MPIDI_do_global_progress()) {
        MPID_THREAD_CS_EXIT(VCI, MPIDI_VCI(vci).lock);
        mpi_errno = MPID_Progress_test(NULL);
        MPID_THREAD_CS_ENTER(VCI, MPIDI_VCI(vci).lock);
    } else {
        mpi_errno = MPIDI_NM_progress(vci, 0);
        MPIR_ERR_CHECK(mpi_errno);
#ifndef MPIDI_CH4_DIRECT_NETMOD
        mpi_errno = MPIDI_SHM_progress(vci, 0);
        MPIR_ERR_CHECK(mpi_errno);
#endif
    }

  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

MPL_STATIC_INLINE_PREFIX void MPID_Progress_start(MPID_Progress_state * state)
{
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPID_PROGRESS_START);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPID_PROGRESS_START);

    MPIDI_progress_state_init(state);
    /* need set count to check for progress_made */
    MPIDI_progress_state_init_count(state);

    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPID_PROGRESS_START);
    return;
}

MPL_STATIC_INLINE_PREFIX void MPID_Progress_end(MPID_Progress_state * state)
{
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPID_PROGRESS_END);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPID_PROGRESS_END);

    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPID_PROGRESS_END);
    return;
}

MPL_STATIC_INLINE_PREFIX int MPID_Progress_test(MPID_Progress_state * state)
{
    if (state == NULL) {
        MPID_Progress_state progress_state;

        MPIDI_progress_state_init(&progress_state);
        return MPIDI_progress_test(&progress_state, 0);
    } else {
        return MPIDI_progress_test(state, 0);
    }
}

MPL_STATIC_INLINE_PREFIX int MPID_Progress_poke(void)
{
    int ret;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPID_PROGRESS_POKE);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPID_PROGRESS_POKE);

    ret = MPID_Progress_test(NULL);

    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPID_PROGRESS_POKE);
    return ret;
}

#if MPICH_THREAD_GRANULARITY == MPICH_THREAD_GRANULARITY__GLOBAL
#define MPIDI_PROGRESS_YIELD() MPID_THREAD_CS_YIELD(GLOBAL, MPIR_THREAD_GLOBAL_ALLFUNC_MUTEX)
#else
#define MPIDI_PROGRESS_YIELD() MPL_thread_yield()
#endif

MPL_STATIC_INLINE_PREFIX int MPID_Progress_wait(MPID_Progress_state * state)
{
    int mpi_errno = MPI_SUCCESS;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPID_PROGRESS_WAIT);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPID_PROGRESS_WAIT);

#ifdef MPIDI_CH4_USE_WORK_QUEUES
    mpi_errno = MPID_Progress_test(state);
    MPIR_ERR_CHECK(mpi_errno);
    MPIDI_PROGRESS_YIELD();

#else
    state->progress_made = 0;
    while (1) {
        mpi_errno = MPIDI_progress_test(state, 1);
        MPIR_ERR_CHECK(mpi_errno);
        if (state->progress_made) {
            break;
        }
        MPIDI_PROGRESS_YIELD();
    }

#endif
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPID_PROGRESS_WAIT);

  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#endif /* CH4_PROGRESS_H_INCLUDED */
