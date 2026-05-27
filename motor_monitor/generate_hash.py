Import("env")
import hashlib
import time

# Create a SHA256 hash based on the current timestamp (simulate a unique build hash)
# In production, you might hash the commit ID or the src folder contents.
hash_object = hashlib.sha256(str(time.time()).encode())
hex_dig = hash_object.hexdigest()

print(f"Injetando FIRMWARE_HASH_HEX dinâmico: {hex_dig}")

# Add the macro to the build environment
env.Append(CPPDEFINES=[
  ("FIRMWARE_HASH_HEX", f'\\"{hex_dig}\\"')
])
