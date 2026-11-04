#include "LivePower.h"

#import <Foundation/Foundation.h>
#import <IOKit/pwr_mgt/IOPMLib.h>

namespace youhost
{
namespace
{

id activityToken = nil;
IOPMAssertionID sleepAssertion = 0;
IOPMAssertionID displayAssertion = 0;

} // namespace

void beginLivePowerClaim() noexcept
{
    @autoreleasepool
    {
        if (activityToken == nil)
        {
            const NSActivityOptions options = NSActivityUserInitiated
                                            | NSActivityIdleSystemSleepDisabled
                                            | NSActivityIdleDisplaySleepDisabled
                                            | NSActivityLatencyCritical;
            id token = [[NSProcessInfo processInfo] beginActivityWithOptions:options
                                                                       reason:@"YouHost live audio"];
#if !__has_feature(objc_arc)
            token = [token retain];
#endif
            activityToken = token;
        }

        if (sleepAssertion == 0)
        {
            IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep,
                                        kIOPMAssertionLevelOn,
                                        CFSTR("YouHost live audio"),
                                        &sleepAssertion);
        }
        if (displayAssertion == 0)
        {
            IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,
                                        kIOPMAssertionLevelOn,
                                        CFSTR("YouHost live audio"),
                                        &displayAssertion);
        }
    }
}

void endLivePowerClaim() noexcept
{
    @autoreleasepool
    {
        if (activityToken != nil)
        {
            [[NSProcessInfo processInfo] endActivity:activityToken];
#if !__has_feature(objc_arc)
            [activityToken release];
#endif
            activityToken = nil;
        }
        if (sleepAssertion != 0)
        {
            IOPMAssertionRelease(sleepAssertion);
            sleepAssertion = 0;
        }
        if (displayAssertion != 0)
        {
            IOPMAssertionRelease(displayAssertion);
            displayAssertion = 0;
        }
    }
}

} // namespace youhost
