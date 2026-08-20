/*
 * FireWireLivesInstaller - activates FireWireLives.dext.
 *
 * A DriverKit extension cannot be loaded from the command line. It must be
 * embedded in an application bundle under Contents/Library/SystemExtensions
 * and activated through OSSystemExtensionRequest, which is what this tool
 * does. macOS then validates the signature and entitlements, asks the user to
 * approve it, and matches it against hardware.
 *
 * Usage:
 *   FireWireLivesInstaller            activate (install or upgrade)
 *   FireWireLivesInstaller deactivate remove the extension
 */

#import <Foundation/Foundation.h>
#import <SystemExtensions/SystemExtensions.h>

static NSString * const kExtensionIdentifier = @"com.firewirelives.FireWireLives";

@interface InstallerDelegate : NSObject <OSSystemExtensionRequestDelegate>
@end

@implementation InstallerDelegate

/* Called when a copy is already installed. Always take the new build: during
 * development the version number rarely changes but the code always does. */
- (OSSystemExtensionReplacementAction)request:(OSSystemExtensionRequest *)request
                   actionForReplacingExtension:(OSSystemExtensionProperties *)existing
                                withExtension:(OSSystemExtensionProperties *)replacement
{
    NSLog(@"replacing installed version %@ with %@",
          existing.bundleShortVersion, replacement.bundleShortVersion);
    return OSSystemExtensionReplacementActionReplace;
}

- (void)requestNeedsUserApproval:(OSSystemExtensionRequest *)request
{
    NSLog(@"⚠️  Awaiting approval.");
    NSLog(@"    Open System Settings > General > Login Items & Extensions,");
    NSLog(@"    find Driver Extensions, and enable FireWireLives.");
}

- (void)request:(OSSystemExtensionRequest *)request
    didFinishWithResult:(OSSystemExtensionRequestResult)result
{
    switch (result) {
    case OSSystemExtensionRequestCompleted:
        NSLog(@"✅ request completed");
        break;
    case OSSystemExtensionRequestWillCompleteAfterReboot:
        NSLog(@"⚠️  will complete after a reboot");
        break;
    default:
        NSLog(@"request finished with result %ld", (long)result);
        break;
    }
    exit(0);
}

- (void)request:(OSSystemExtensionRequest *)request didFailWithError:(NSError *)error
{
    NSLog(@"❌ request failed: %@ (domain %@, code %ld)",
          error.localizedDescription, error.domain, (long)error.code);

    /* The common development failures, translated into what to actually do. */
    switch (error.code) {
    case 8:  /* OSSystemExtensionErrorCodeSignatureInvalid */
        NSLog(@"    Signature rejected. Confirm SIP is disabled and");
        NSLog(@"    `systemextensionsctl developer on` is set.");
        break;
    case 9:  /* OSSystemExtensionErrorForbiddenBySystemPolicy */
        NSLog(@"    Blocked by system policy - approve it in System Settings.");
        break;
    default:
        break;
    }
    exit(1);
}

@end

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        BOOL deactivate = (argc > 1 && strcmp(argv[1], "deactivate") == 0);

        InstallerDelegate *delegate = [[InstallerDelegate alloc] init];
        OSSystemExtensionRequest *request;

        if (deactivate) {
            NSLog(@"deactivating %@", kExtensionIdentifier);
            request = [OSSystemExtensionRequest
                deactivationRequestForExtension:kExtensionIdentifier
                                          queue:dispatch_get_main_queue()];
        } else {
            NSLog(@"activating %@", kExtensionIdentifier);
            request = [OSSystemExtensionRequest
                  activationRequestForExtension:kExtensionIdentifier
                                          queue:dispatch_get_main_queue()];
        }

        request.delegate = delegate;
        [[OSSystemExtensionManager sharedManager] submitRequest:request];

        /* The request is asynchronous; the delegate calls exit(). */
        [[NSRunLoop mainRunLoop] run];
    }
    return 0;
}
