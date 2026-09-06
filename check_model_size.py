import os
import numpy as np
import tensorflow as tf
from train_hey_vaani_kws import build_ds_cnn, INPUT_SHAPE

model = build_ds_cnn()

def representative_dataset_gen():
    for _ in range(100):
        data = np.random.rand(1, INPUT_SHAPE[0], INPUT_SHAPE[1], INPUT_SHAPE[2])
        yield [data.astype(np.float32)]

converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset = representative_dataset_gen
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type = tf.int8
converter.inference_output_type = tf.int8

tflite_model = converter.convert()

with open('dummy.tflite', 'wb') as f:
    f.write(tflite_model)

size = os.path.getsize('dummy.tflite')
print(f"Model size is {size} bytes ({size/1024:.2f} KB)")
