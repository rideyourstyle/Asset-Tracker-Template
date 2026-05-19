pipeline {
    agent any

    options {
        // Checkout is done manually into project/ so west workspace root = WORKSPACE
        skipDefaultCheckout(true)
    }

    parameters {
        booleanParam(name: 'CLEAN_WORKSPACE',
                     defaultValue: false,
                     description: 'Delete the entire NCS west workspace and re-fetch all SDK ' +
                                  'dependencies (nrf, zephyr, modules, ...). Takes ~10–20 min.')
        booleanParam(name: 'PRISTINE',
                     defaultValue: false,
                     description: 'Full firmware rebuild (--pristine). Required after Kconfig changes.')
        booleanParam(name: 'FLASH',
                     defaultValue: false,
                     description: 'Flash the connected Thingy:91 X after a successful build.')
    }

    environment {
        NRFUTIL     = '/home/peter/opt/nrfutil'
        NCS_VERSION = 'v3.1.1'
        BOARD       = 'thingy91x/nrf9151/ns'
        OVERLAY     = 'overlay-rest.conf'
        APP_DIR     = 'project/app'
        BUILD_DIR   = 'project/app/build'
        FIRMWARE    = 'project/app/build/app_image.hex'
    }

    stages {

        // ------------------------------------------------------------------ //
        // 1. Checkout the manifest repo into project/                         //
        //    → WORKSPACE becomes the west workspace root                      //
        // ------------------------------------------------------------------ //
        stage('Checkout') {
            steps {
                dir('project') {
                    checkout scm
                }
            }
        }

        // ------------------------------------------------------------------ //
        // 2. Set up the west workspace                                        //
        //    • CLEAN_WORKSPACE=true  → delete everything except project/,    //
        //                              then run west init + west update       //
        //    • CLEAN_WORKSPACE=false → skip init if .west/ already exists,   //
        //                              always run west update (incremental)   //
        // ------------------------------------------------------------------ //
        stage('West setup') {
            steps {
                script {
                    if (params.CLEAN_WORKSPACE) {
                        echo 'Cleaning NCS west workspace (keeping project/)...'
                        sh '''
                            find "${WORKSPACE}" -maxdepth 1 -mindepth 1 \
                                -not -name 'project' \
                                -exec rm -rf {} +
                        '''
                    }
                }

                sh '''
                    if [ ! -d .west ]; then
                        echo "Initializing west workspace..."
                        ${NRFUTIL} toolchain-manager launch --ncs-version ${NCS_VERSION} -- \
                            west init -l project/
                    else
                        echo "west workspace already initialized, skipping west init."
                    fi
                '''

                sh '''
                    echo "Running west update..."
                    ${NRFUTIL} toolchain-manager launch --ncs-version ${NCS_VERSION} -- \
                        west update --narrow --fetch-opt="--depth=1"
                '''
            }
        }

        // ------------------------------------------------------------------ //
        // 3. Build the firmware                                               //
        // ------------------------------------------------------------------ //
        stage('Build') {
            steps {
                script {
                    def pristineFlag = params.PRISTINE ? '--pristine' : ''
                    sh """
                        ${NRFUTIL} toolchain-manager launch --ncs-version ${NCS_VERSION} -- \
                            west build ${pristineFlag} \
                                -b ${BOARD} \
                                -d ${BUILD_DIR} \
                                ${APP_DIR} \
                                -- -DEXTRA_CONF_FILE=${OVERLAY}
                    """
                }
            }
            post {
                success {
                    archiveArtifacts artifacts: "${FIRMWARE}", fingerprint: true
                    echo "Firmware archived: ${FIRMWARE}"
                }
            }
        }

        // ------------------------------------------------------------------ //
        // 4. Flash (only when FLASH=true and a device is connected)           //
        // ------------------------------------------------------------------ //
        stage('Flash') {
            when {
                expression { params.FLASH }
            }
            steps {
                sh """
                    ${NRFUTIL} device program \
                        --firmware ${FIRMWARE} \
                        --traits mcuBoot \
                        --options target=nRF91
                """
            }
        }
    }

    post {
        always {
            echo "Pipeline finished: ${currentBuild.currentResult}"
        }
        failure {
            echo "Build failed — check the console output above for details."
        }
    }
}
