// A virtual monitor made with CoreGraphics' undocumented CGVirtualDisplay classes, wrapped so Swift never touches
// them directly. The display lasts until -remove, or until the process exits.
#pragma once
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface TDVirtualDisplay : NSObject

// The private classes' methods and properties with their type encodings, to check against what the shim assumes.
+ (NSString *)describePrivateClasses;

// modes holds @[width, height, refreshHz] for each mode. Fails if the classes are missing or the WindowServer
// refuses the display or its settings.
- (nullable instancetype)initWithName:(NSString *)name
                        maxPixelsWide:(uint32_t)maxPixelsWide
                        maxPixelsHigh:(uint32_t)maxPixelsHigh
                    sizeInMillimeters:(CGSize)sizeInMillimeters
                             vendorID:(uint32_t)vendorID
                            productID:(uint32_t)productID
                         serialNumber:(uint32_t)serialNumber
                                hiDPI:(BOOL)hiDPI
                                modes:(NSArray<NSArray<NSNumber *> *> *)modes
                                error:(NSError **)error;

@property(nonatomic, readonly) CGDirectDisplayID displayID;

// Lets go of the display. It's removed asynchronously, after this returns.
- (void)remove;

@end

// Reads or writes any numbered field of an event, including the undocumented ones Swift's CGEventField can't name.
int64_t TDEventInteger(CGEventRef event, uint32_t field);
double TDEventDouble(CGEventRef event, uint32_t field);
void TDEventSetInteger(CGEventRef event, uint32_t field, int64_t value);
void TDEventSetDouble(CGEventRef event, uint32_t field, double value);

NS_ASSUME_NONNULL_END
