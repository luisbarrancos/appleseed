#!/usr/bin/env python

import os
import subprocess

reference_Lab = []
sampleval_Lab = []
deltaE = []
samples = 0

with open("color_deltaE_CIE2000.txt", 'r') as f:
    for line in f:
        numbers = [float(num) for num in line.split()]
        
        reference = [numbers[0], numbers[1], numbers[2]]
        sample = [numbers[3], numbers[4], numbers[5]]
        delta = numbers[6]

        reference_Lab.append(reference)
        sampleval_Lab.append(sample)
        deltaE.append(delta)
        samples += 1

def osltest_command(i) :

    reference = str(reference_Lab[i]).strip('[]')
    reference = reference.replace(' ', '')

    sample = str(sampleval_Lab[i]).strip('[]')
    sample = sample.replace(' ', '')

    delta = str(deltaE[i]).strip('[]')

    command = "testshade -g 1 1 "
    command += "--param:type=color reference_Lab " + reference + " "
    command += "--param:type=color sampleval_Lab " + sample + " "
    command += "--param:type=float dE " + delta + " "
    command += "deltaE_00"
    return command

for i in range(0,samples):
    cmdret = subprocess.call(osltest_command(i), shell=True, env=None)

