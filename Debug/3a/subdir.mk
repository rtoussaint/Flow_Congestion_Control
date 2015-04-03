################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../3a/reliable.c \
../3a/rlib.c 

O_SRCS += \
../3a/reliable.o \
../3a/rlib.o 

OBJS += \
./3a/reliable.o \
./3a/rlib.o 

C_DEPS += \
./3a/reliable.d \
./3a/rlib.d 


# Each subdirectory must supply rules for building sources it contributes
3a/%.o: ../3a/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler'
	gcc -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


