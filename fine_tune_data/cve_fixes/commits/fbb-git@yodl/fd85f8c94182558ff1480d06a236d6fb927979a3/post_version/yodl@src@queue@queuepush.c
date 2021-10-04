#include "queue.ih"

void queue_push(register Queue *qp, size_t extra_length, char const *info)
{
    register char *cp;
    size_t memory_length;
    size_t available_length;
    size_t begin_length;
    size_t n_begin;
    size_t q_length;

    if (!extra_length)
        return;

    memory_length    = qp->d_memory_end - qp->d_memory;

    q_length = 
        qp->d_read <= qp->d_write ?
            (size_t)(qp->d_write - qp->d_read)
        :
            memory_length - (qp->d_read - qp->d_write);

    available_length = memory_length - q_length - 1;
                            /* -1, as the Q cannot completely fill up all   */
                            /* available memory in the buffer               */

    if (message_show(MSG_INFO))
        message("push_front %u bytes in `%s'", (unsigned)extra_length, info);

    if (extra_length > available_length)
    {
        size_t original_length = memory_length;

                                                   /* enlarge the buffer:  */
        memory_length += extra_length - available_length + BLOCK_QUEUE;

        cp = new_memory(memory_length, sizeof(char));

        if (message_show(MSG_INFO))
            message("Reallocating queue at %p to %p", qp->d_memory, cp);

        if (qp->d_read > qp->d_write)               /* q wraps around end   */
        {
            size_t tail_len = qp->d_memory_end - qp->d_read;
            memcpy(cp, qp->d_read, tail_len);       /* first part -> begin  */
                                                    /* 2nd part beyond      */
            memcpy(cp + tail_len, qp->d_memory, 
                                    (size_t)(qp->d_write - qp->d_memory));
            qp->d_write = cp + q_length;
            qp->d_read = cp;
        }
        else                                        /* q as one block       */
        {
            memcpy(cp, qp->d_memory, original_length);/* cp existing buffer   */
            qp->d_read = cp + (qp->d_read - qp->d_memory);
            qp->d_write = cp + (qp->d_write - qp->d_memory);
        }

        free(qp->d_memory);                         /* free old memory      */
        qp->d_memory_end = cp + memory_length;      /* update d_memory_end  */
        qp->d_memory = cp;                          /* update d_memory      */
    }

    /*
        Write as much as possible at the begin of the buffer, then write
        the remaining chars at the end.

        q_length is increased by the length of the info string

        The first chars to write are at the end of info, and the 2nd part to
        write are the initial chars of info, since the initial part of info
        is then read first.
    */

                                                /* # chars available at the */
    begin_length = qp->d_read - qp->d_memory;   /* begin of the buffer      */

    n_begin = extra_length <= begin_length ?    /* determine # to write at  */
                    extra_length                /* the begin of the buffer  */
                :
                    begin_length;

    memcpy                                      /* write trailing part of   */
    (                                           /* info first               */
        qp->d_read -= n_begin,
        info + extra_length - n_begin,
        n_begin
    );

    if (extra_length > begin_length)            /* not yet all chars written*/
    {
        /* continue with the remaining number of characters. Insert these at*/
        /* the end of the buffer                                            */

        extra_length -= begin_length;           /* reduce # to write        */


        memcpy                                  /* d_read wraps to the end  */
        (                                       /* write info's rest        */
            qp->d_read = qp->d_memory_end - extra_length,
            info,
            extra_length
        );
    }
}
