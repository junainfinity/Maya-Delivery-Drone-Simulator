#pragma once

#include "Commandlets/Commandlet.h"
#include "DDSN05RobotGeometryCommandlet.generated.h"

// Editor-only diagnostic. It loads existing packages and writes one external
// JSON report; it never saves or edits an Unreal package.
UCLASS()
class UDDSN05RobotGeometryCommandlet final : public UCommandlet
{
    GENERATED_BODY()
public:
    UDDSN05RobotGeometryCommandlet();
    virtual int32 Main(const FString& Params) override;
};
