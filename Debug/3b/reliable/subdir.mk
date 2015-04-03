################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../3b/reliable/reliable.c \
../3b/reliable/rlib.c 

O_SRCS += \
../3b/reliable/reliable.o 

OBJS += \
./3b/reliable/reliable.o \
./3b/reliable/rlib.o 

C_DEPS += \
./3b/reliable/reliable.d \
./3b/reliable/rlib.d 


# Each subdirectory must supply rules for building sources it contributes
3b/reliable/%.o: ../3b/reliable/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler'
	gcc -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


