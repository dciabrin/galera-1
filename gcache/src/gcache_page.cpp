/*
 * Copyright (C) 2010 Codership Oy <info@codership.com>
 */

/*! @file page file class implementation */

#include "gcache_page.hpp"

// for posix_fadvise()
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#include <sys/fcntl.h>

static ssize_t
check_size (ssize_t size)
{
    if (size < 0)
        gu_throw_error(EINVAL) << "Negative page size: " << size;

    return (size + sizeof(gcache::BufferHeader));
}

void
gcache::Page::reset () throw ()
{
    if (gu_unlikely (used_ > 0))
    {
        log_fatal << "Attempt to reset a page '" << name()
                  << "' used by " << used_ << " buffers. Aborting.";
        abort();
    }

    space_ = mmap_.size;
    next_  = static_cast<uint8_t*>(mmap_.ptr);
}

void
gcache::Page::drop_fs_cache() const throw()
{
    mmap_.dont_need();

    int const err (posix_fadvise (fd_.get(), 0, fd_.get_size(),
                                  POSIX_FADV_DONTNEED));
    if (err != 0)
    {
        log_warn << "Failed to set POSIX_FADV_DONTNEED on " << fd_.get_name()
                 << ": " << err << " (" << strerror(err) << ")";
    }
}

gcache::Page::Page (const std::string& name, ssize_t size) throw (gu::Exception)
    :
    fd_   (name, check_size(size), false, false),
    mmap_ (fd_),
    next_ (static_cast<uint8_t*>(mmap_.ptr)),
    space_(mmap_.size),
    used_ (0)
{
    log_info << "Created a temporary page " << name << "of size " << space_
             << " bytes";
    BH_clear (reinterpret_cast<BufferHeader*>(next_));
}

void*
gcache::Page::malloc (ssize_t size) throw ()
{
    ssize_t const buf_size (size + sizeof(BufferHeader));

    if (buf_size <= space_)
    {
        BufferHeader* bh(BH_cast(next_));

        bh->size  = buf_size;
        bh->seqno = SEQNO_NONE;
        bh->ctx   = this;
        bh->flags = 0;
        bh->store = BUFFER_IN_PAGE;

        space_ -= buf_size;
        next_  += buf_size;
        used_++;

#ifndef NDEBUG
        if (space_ >= static_cast<ssize_t>(sizeof(BufferHeader)))
        {
            BH_clear (BH_cast(next_));
            assert (reinterpret_cast<uint8_t*>(bh + 1) < next_);
        }

        assert (next_ <= static_cast<uint8_t*>(mmap_.ptr) + mmap_.size);
#endif
        return (bh + 1);
    }
    else
    {
        log_debug << "Failed to allocate " << buf_size << " bytes, space left: "
                  << space_ << " bytes, total allocated: "
                  << next_ - static_cast<uint8_t*>(mmap_.ptr);
        return 0;
    }
}

void*
gcache::Page::realloc (void* ptr, ssize_t size) throw ()
{
    BufferHeader* bh(ptr2BH(ptr));

    ssize_t const old_size (bh->size - sizeof(BufferHeader));

    if (bh == BH_cast(next_ - bh->size)) // last buffer, can shrink and expand
    {
        ssize_t const diff_size (size - old_size);

        if (gu_likely (diff_size < space_))
        {
            bh->size += diff_size;
            space_   -= diff_size;
            next_    += diff_size;
            BH_clear (BH_cast(next_));

            return ptr;
        }
        else return 0; // not enough space in this page
    }
    else
    {
        if (gu_likely(size > old_size))
        {
            void* const ret (malloc (size));

            if (ret)
            {
                memcpy (ret, ptr, old_size);
                used_--;
            }

            return ret;
        }
        else
        {
            // do nothing, we can't shrink the buffer, it is locked
            return ptr;
        }
    }
}
