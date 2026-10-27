#ifndef BOLTPACK_TEST_COREFOUNDATION_H
#define BOLTPACK_TEST_COREFOUNDATION_H

typedef const void *CFStringRef;
typedef void *CFMutableStringRef;
typedef long CFIndex;
typedef const void *CFAllocatorRef;
typedef unsigned long CFStringEncoding;
typedef unsigned long CFStringCompareFlags;
typedef unsigned long CFStringNormalizationForm;

#define kCFAllocatorDefault ((CFAllocatorRef)0)
#define kCFStringEncodingUTF8 ((CFStringEncoding)0)
#define kCFCompareCaseInsensitive ((CFStringCompareFlags)1)
#define kCFStringNormalizationFormD ((CFStringNormalizationForm)0)

CFStringRef CFStringCreateWithCString(CFAllocatorRef allocator,
                                      const char *c_string,
                                      CFStringEncoding encoding);
CFMutableStringRef CFStringCreateMutableCopy(CFAllocatorRef allocator,
                                             CFIndex max_length,
                                             CFStringRef string);
void CFRelease(const void *object);
void CFStringNormalize(CFMutableStringRef string,
                       CFStringNormalizationForm form);
void CFStringFold(CFMutableStringRef string, CFStringCompareFlags flags,
                  void *locale);
CFIndex CFStringGetLength(CFStringRef string);
CFIndex CFStringGetMaximumSizeForEncoding(CFIndex length,
                                          CFStringEncoding encoding);
int CFStringGetCString(CFStringRef string, char *buffer, CFIndex buffer_size,
                       CFStringEncoding encoding);

#endif
