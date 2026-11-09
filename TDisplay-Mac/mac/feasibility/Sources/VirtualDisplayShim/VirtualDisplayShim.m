#import "VirtualDisplayShim.h"

#import <objc/runtime.h>

// What the private classes look like, as +describePrivateClasses showed them on macOS 27.2.
// The classes are looked up by name, so nothing here has to link against them. +describePrivateClasses shows
// what the running macOS actually has.
@class CGVirtualDisplay;

@interface CGVirtualDisplayDescriptor : NSObject
@property(retain, nonatomic) dispatch_queue_t queue;
@property(retain, nonatomic) NSString *name;
@property(nonatomic) unsigned int maxPixelsHigh;
@property(nonatomic) unsigned int maxPixelsWide;
@property(nonatomic) CGSize sizeInMillimeters;
@property(nonatomic) unsigned int serialNum;
@property(nonatomic) unsigned int productID;
@property(nonatomic) unsigned int vendorID;
@property(nonatomic) CGPoint redPrimary;
@property(nonatomic) CGPoint greenPrimary;
@property(nonatomic) CGPoint bluePrimary;
@property(nonatomic) CGPoint whitePoint;
@property(copy, nonatomic) void (^terminationHandler)(id, CGVirtualDisplay *);
@end

@interface CGVirtualDisplayMode : NSObject
@property(readonly, nonatomic) double refreshRate;
@property(readonly, nonatomic) unsigned int height;
@property(readonly, nonatomic) unsigned int width;
- (instancetype)initWithWidth:(unsigned int)width height:(unsigned int)height refreshRate:(double)refreshRate;
@end

@interface CGVirtualDisplaySettings : NSObject
@property(retain, nonatomic) NSArray *modes;
@property(nonatomic) unsigned int hiDPI;
@end

@interface CGVirtualDisplay : NSObject
@property(readonly, nonatomic) unsigned int displayID;
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor *)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings *)settings;
@end

static NSString *const kErrorDomain = @"TDVirtualDisplay";

static NSError *makeError(NSInteger code, NSString *message) {
    return [NSError errorWithDomain:kErrorDomain code:code userInfo:@{NSLocalizedDescriptionKey: message}];
}

static void describeClass(NSMutableString *out, NSString *name) {
    Class cls = NSClassFromString(name);
    if (!cls) {
        [out appendFormat:@"%@: missing\n", name];
        return;
    }
    [out appendFormat:@"%@ (superclass %@)\n", name, NSStringFromClass(class_getSuperclass(cls))];
    unsigned int count = 0;
    objc_property_t *properties = class_copyPropertyList(cls, &count);
    for (unsigned int i = 0; i < count; ++i) {
        [out appendFormat:@"  property %s  %s\n", property_getName(properties[i]), property_getAttributes(properties[i])];
    }
    free(properties);
    for (int meta = 0; meta < 2; ++meta) {
        Method *methods = class_copyMethodList(meta ? object_getClass(cls) : cls, &count);
        for (unsigned int i = 0; i < count; ++i) {
            [out appendFormat:@"  %c%s  %s\n", meta ? '+' : '-', sel_getName(method_getName(methods[i])), method_getTypeEncoding(methods[i])];
        }
        free(methods);
    }
}

@implementation TDVirtualDisplay {
    CGVirtualDisplay *_display;
}

+ (NSString *)describePrivateClasses {
    NSMutableString *out = [NSMutableString string];
    for (NSString *name in @[@"CGVirtualDisplayDescriptor", @"CGVirtualDisplayMode", @"CGVirtualDisplaySettings", @"CGVirtualDisplay"]) {
        describeClass(out, name);
    }
    return out;
}

- (nullable instancetype)initWithName:(NSString *)name
                        maxPixelsWide:(uint32_t)maxPixelsWide
                        maxPixelsHigh:(uint32_t)maxPixelsHigh
                    sizeInMillimeters:(CGSize)sizeInMillimeters
                             vendorID:(uint32_t)vendorID
                            productID:(uint32_t)productID
                         serialNumber:(uint32_t)serialNumber
                                hiDPI:(BOOL)hiDPI
                                modes:(NSArray<NSArray<NSNumber *> *> *)modes
                                error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    Class descriptorClass = NSClassFromString(@"CGVirtualDisplayDescriptor");
    Class modeClass = NSClassFromString(@"CGVirtualDisplayMode");
    Class settingsClass = NSClassFromString(@"CGVirtualDisplaySettings");
    Class displayClass = NSClassFromString(@"CGVirtualDisplay");
    if (!descriptorClass || !modeClass || !settingsClass || !displayClass) {
        if (error) *error = makeError(1, @"this macOS has no CGVirtualDisplay classes");
        return nil;
    }
    if (![modeClass instancesRespondToSelector:@selector(initWithWidth:height:refreshRate:)] ||
        ![displayClass instancesRespondToSelector:@selector(initWithDescriptor:)] ||
        ![displayClass instancesRespondToSelector:@selector(applySettings:)]) {
        if (error) *error = makeError(2, @"the CGVirtualDisplay classes lack a method this relies on");
        return nil;
    }
    @try {
        CGVirtualDisplayDescriptor *descriptor = [[descriptorClass alloc] init];
        descriptor.queue = dispatch_queue_create("tdisplay.virtual-display", DISPATCH_QUEUE_SERIAL);
        descriptor.name = name;
        descriptor.maxPixelsWide = maxPixelsWide;
        descriptor.maxPixelsHigh = maxPixelsHigh;
        descriptor.sizeInMillimeters = sizeInMillimeters;
        descriptor.vendorID = vendorID;
        descriptor.productID = productID;
        descriptor.serialNum = serialNumber;
        // BT.709 primaries and D65, which is what the tablet shows.
        descriptor.redPrimary = CGPointMake(0.640, 0.330);
        descriptor.greenPrimary = CGPointMake(0.300, 0.600);
        descriptor.bluePrimary = CGPointMake(0.150, 0.060);
        descriptor.whitePoint = CGPointMake(0.3127, 0.3290);
        descriptor.terminationHandler = ^(id sender, CGVirtualDisplay *display) {
            fprintf(stderr, "virtual display: the WindowServer ended it\n");
        };
        _display = [[displayClass alloc] initWithDescriptor:descriptor];
        if (!_display) {
            if (error) *error = makeError(3, @"initWithDescriptor: made no display");
            return nil;
        }
        NSMutableArray *displayModes = [NSMutableArray array];
        for (NSArray<NSNumber *> *mode in modes) {
            [displayModes addObject:[[modeClass alloc] initWithWidth:mode[0].unsignedIntValue height:mode[1].unsignedIntValue refreshRate:mode[2].doubleValue]];
        }
        CGVirtualDisplaySettings *settings = [[settingsClass alloc] init];
        settings.hiDPI = hiDPI ? 1 : 0;
        settings.modes = displayModes;
        if (![_display applySettings:settings]) {
            _display = nil;
            if (error) *error = makeError(4, @"the WindowServer refused the settings (applySettings: returned NO)");
            return nil;
        }
    } @catch (NSException *exception) {
        _display = nil;
        if (error) *error = makeError(5, [NSString stringWithFormat:@"%@: %@", exception.name, exception.reason]);
        return nil;
    }
    return self;
}

- (CGDirectDisplayID)displayID {
    return _display ? _display.displayID : kCGNullDirectDisplay;
}

- (void)remove {
    _display = nil;
}

@end

int64_t TDEventInteger(CGEventRef event, uint32_t field) { return CGEventGetIntegerValueField(event, (CGEventField)field); }
double TDEventDouble(CGEventRef event, uint32_t field) { return CGEventGetDoubleValueField(event, (CGEventField)field); }
void TDEventSetInteger(CGEventRef event, uint32_t field, int64_t value) { CGEventSetIntegerValueField(event, (CGEventField)field, value); }
void TDEventSetDouble(CGEventRef event, uint32_t field, double value) { CGEventSetDoubleValueField(event, (CGEventField)field, value); }
