#!/usr/bin/env python3

import numpy as np
import matplotlib.pyplot as plt

mtu = 1500
overhead = 120
max_s = 65535

# Define the loss rates instead of prompting
loss_rates_fixed = [0.001, 0.01, 0.5, 0.1]

# Vectorized size array
s = np.arange(1, max_s)

# Vectorized calculation for load
def calcul(size, loss_rate):
    return (2 - (1 - loss_rate) ** np.maximum(size / mtu, 1)) * (1 + overhead / size)

# Create the first figure for load vs size at fixed loss rates
plt.figure(figsize=(10, 5))

# Subplot 1: Load vs size for fixed loss rates
plt.subplot(1, 2, 1)
for loss_rate in loss_rates_fixed:
    load = calcul(s, loss_rate)
    plt.plot(s, load, label=f'Loss rate={loss_rate}')
    min_load_index = np.argmin(load)
    min_size = s[min_load_index]
    print(f"The size for which the load is minimum: {min_size} at {loss_rate*100}% loss rate")
    
plt.xscale('log')
plt.xlabel("UDP Packet size (Bytes)")
plt.ylabel("Load")
plt.title("Load vs Size for Different Loss Rates")
plt.grid(True)
plt.legend()




# Subplot 2: Load vs loss rate for specified sizes
sizes = [100, 400, 1200, 1500, 4000, 10000]
loss_rates = np.logspace(-4, 0, 100)

plt.subplot(1, 2, 2)
for size in sizes:
    load = calcul(size, loss_rates)  # Calculate load for each loss rate
    plt.plot(loss_rates, load, label=f'Size={size} Bytes')

plt.xscale('log')
plt.xlabel("Loss Rate")
plt.ylabel("Load")
plt.title("Load vs Loss Rate for Different Sizes")
plt.grid(True)
plt.legend()

plt.tight_layout()  # Adjust layout for clarity
plt.show()
