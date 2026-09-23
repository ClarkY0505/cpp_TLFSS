#ifndef __INC_COMMON_NOCOPY_H__
#define __INC_COMMON_NOCOPY_H__

class NoCopy{
public:
    NoCopy(const NoCopy &) = delete;
    NoCopy & operator=(const NoCopy &) = delete;

protected:
    NoCopy() = default;
    ~NoCopy() = default;
};

#endif // __INC_COMMON_NOCOPY_H__
