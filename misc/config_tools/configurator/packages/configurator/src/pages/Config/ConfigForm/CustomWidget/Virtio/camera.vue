<template>
  <div style="border: 2px solid #cccccc;padding: 12px 50px 12px 6px;border-radius: 5px;margin-bottom: 1rem;"
       v-if="defaultVal[0].virtual_camera && defaultVal[0].virtual_camera.length>0">
    <b style="margin-bottom: 2rem">Virtio camera device</b>
    <div class="p-4">
      <b-row class="virtio_camera" v-for="(cam,index) in this.defaultVal[0].virtual_camera">
        <b-col class="p-4">
          <label>
            <n-popover trigger="hover" placement="top-start">
              <template #trigger>
                <IconInfo/>
              </template>
              <span v-html="this.VirtualCameraType.properties.physical_camera_id.description"></span>
            </n-popover>
            Physical Camera
          </label>
        </b-col>
        <b-col>
          <b-form-select v-model="cam.physical_camera_id" :options="PhysicalCameraIDs"/>
        </b-col>
        <b-col class="p-4">
          <label>
            <n-popover trigger="hover" placement="top-start">
              <template #trigger>
                <IconInfo/>
              </template>
              <span v-html="this.VirtualCameraType.properties.resolution.description"></span>
            </n-popover>
            Stream Size
          </label>
        </b-col>
        <b-col md="3">
          <b-form-select v-model="cam.resolution" :options="getCameraResolutionsByID(cam.physical_camera_id)"/>
        </b-col>
        <b-col>
          <b-col class="p-4" v-model="cam.virtual_camera_id" > Virtual Camera {{ index }}</b-col>
        </b-col>
        <b-col>
          <div class="ToolSet">
            <div @click="removeCamera(index)">
              <Icon size="18px">
                <Minus/>
              </Icon>
            </div>
            <div @click="addCamera(index)">
              <Icon size="18px">
                <Plus/>
              </Icon>
            </div>
          </div>
        </b-col>
      </b-row>
    </div>
  </div>
  <div v-else>
    <b style="margin-bottom: 2rem">Virtio camera device</b>
      <div class="ToolSet">
        <div @click="addCamera(index)">
          <Icon size="18px">
            <Plus/>
          </Icon>
        </div>
      </div>
  </div>
</template>

<script>
import {
  fieldProps,
  vueUtils,
  formUtils,
  schemaValidate
} from '@lljj/vue3-form-naive';
import _ from 'lodash'
import {Icon} from "@vicons/utils";
import {Plus, Minus} from '@vicons/fa'
import {BCol, BFormInput, BRow} from "bootstrap-vue-3";
import IconInfo from '@lljj/vjsf-utils/icons/IconInfo.vue';

export default {
  name: "camera",
  components: {BCol, BRow, BFormInput, Icon, Plus, Minus, IconInfo},
  props: {
    ...fieldProps,
  },
  watch: {
    rootFormData: {
      handler(newValue, oldValue) {
        this.defaultVal = vueUtils.getPathVal(newValue, this.curNodePath)
      },
      deep: true
    },
    defaultVal: {
      handler(newValue, oldValue) {
        vueUtils.setPathVal(this.rootFormData, this.curNodePath, newValue);
      },
      deep: true
    }
  },
  data() {
    return {
      VirtualCameraType: this.rootSchema.definitions['VirtualCameraType'],
      PhysicalCameraIDs: this.rootSchema.definitions['VirtualCameraType']['properties']['physical_camera_id']['enum'],
      CameraResolutions: this.rootSchema.definitions['VirtualCameraType']['properties']['resolution']['enum'],
      defaultVal: vueUtils.getPathVal(this.rootFormData, this.curNodePath)
    }
  },
  methods: {
    getCameraResolutionsByID(cameraID){
      let resolutions = []
      this.CameraResolutions.forEach((item, i) => {
        if (item.startsWith(cameraID)){
          let resolution = item.split(': ')[1]
          resolutions.push(resolution)
        }
    })
      return resolutions
    },
    addCamera(index) {
      if (index == undefined){
        index = -1
      }
      this.defaultVal[0].virtual_camera.splice(index + 1, 0, {physical_camera_id: null, resolution: null, virtual_camera_id: index + 1})
    },
    removeCamera(index) {
      this.defaultVal[0].virtual_camera.splice(index, 1)
    }
  }
}
</script>

<style scoped>
.ToolSet {
  display: flex;
  flex-direction: row;
  justify-content: space-around;
  margin: 1rem;
  gap: 0.5rem;
  max-width: 5rem;
}
.ToolSet div {
  cursor: pointer;
  border: 1px solid gray;
  border-radius: 3px;
  background: #f9f9f9;
  padding: 5px 5px 3px;
}
</style>
