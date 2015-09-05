/*  Ekos
    Copyright (C) 2012 Jasem Mutlaq <mutlaqja@ikarustech.com>

    This application is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
 */

#include "guide.h"

#include <QDateTime>

#include "guide/gmath.h"
#include "guide/guider.h"
#include "Options.h"

#include <KMessageBox>
#include <KLed>
#include <KLocalizedString>

#include "indi/driverinfo.h"
#include "fitsviewer/fitsviewer.h"
#include "fitsviewer/fitsview.h"

#include "guide/rcalibration.h"
#include "guideadaptor.h"

#include <basedevice.h>

namespace Ekos
{

Guide::Guide() : QWidget()
{
    setupUi(this);

    new GuideAdaptor(this);
    QDBusConnection::sessionBus().registerObject("/KStars/Ekos/Guide",  this);

    currentCCD = NULL;
    currentTelescope = NULL;
    ccd_hor_pixel =  ccd_ver_pixel =  focal_length =  aperture = -1;
    useGuideHead = false;
    useDarkFrame = false;
    rapidGuideReticleSet = false;
    isSuspended = false;
    darkExposure = 0;
    darkImage = NULL;
    AODriver= NULL;
    GuideDriver=NULL;

    guideDeviationRA = guideDeviationDEC = 0;

    tabWidget = new QTabWidget(this);

    tabLayout->addWidget(tabWidget);

    guiderStage = CALIBRATION_STAGE;

    pmath = new cgmath();

    connect(pmath, SIGNAL(newAxisDelta(double,double)), this, SIGNAL(newAxisDelta(double,double)));
    connect(pmath, SIGNAL(newAxisDelta(double,double)), this, SLOT(updateGuideDriver(double,double)));

    calibration = new rcalibration(pmath, this);

    guider = new rguider(pmath, this);

    connect(guider, SIGNAL(ditherToggled(bool)), this, SIGNAL(ditherToggled(bool)));
    connect(guider, SIGNAL(autoGuidingToggled(bool,bool)), this, SIGNAL(autoGuidingToggled(bool,bool)));
    connect(guider, SIGNAL(ditherComplete()), this, SIGNAL(ditherComplete()));

    tabWidget->addTab(calibration, calibration->windowTitle());
    tabWidget->addTab(guider, guider->windowTitle());
    tabWidget->setTabEnabled(1, false);

    connect(ST4Combo, SIGNAL(currentIndexChanged(int)), this, SLOT(newST4(int)));
    connect(guiderCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(checkCCD(int)));

    foreach(QString filter, FITSViewer::filterTypes)
        filterCombo->addItem(filter);
}

Guide::~Guide()
{
    delete guider;
    delete calibration;
    delete pmath;
}

void Guide::addCCD(ISD::GDInterface *newCCD, bool isPrimaryGuider)
{
    currentCCD = (ISD::CCD *) newCCD;

    CCDs.append(currentCCD);

    guiderCombo->addItem(currentCCD->getDeviceName());

    if (isPrimaryGuider)
    {
        checkCCD(CCDs.count()-1);
        guiderCombo->setCurrentIndex(CCDs.count()-1);
    }
    else
    {
        checkCCD(0);
        guiderCombo->setCurrentIndex(0);
    }

    //if (currentCCD->hasGuideHead())
       // addGuideHead(newCCD);


    //qDebug() << "SetCCD: ccd_pix_w " << ccd_hor_pixel << " - ccd_pix_h " << ccd_ver_pixel << " - focal length " << focal_length << " aperture " << aperture;

}

void Guide::setTelescope(ISD::GDInterface *newTelescope)
{
    currentTelescope = (ISD::Telescope*) newTelescope;

    syncTelescopeInfo();

}

bool Guide::setCCD(QString device)
{
    for (int i=0; i < guiderCombo->count(); i++)
        if (device == guiderCombo->itemText(i))
        {
            checkCCD(i);
            return true;
        }

    return false;
}

void Guide::checkCCD(int ccdNum)
{
    if (ccdNum == -1)
        ccdNum = guiderCombo->currentIndex();

    if (ccdNum <= CCDs.count())
    {
        currentCCD = CCDs.at(ccdNum);

        connect(currentCCD, SIGNAL(FITSViewerClosed()), this, SLOT(viewerClosed()), Qt::UniqueConnection);

        if (currentCCD->hasGuideHead() && guiderCombo->currentText().contains("Guider"))
            useGuideHead=true;
        else
            useGuideHead=false;

        syncCCDInfo();
    }
}

void Guide::addGuideHead(ISD::GDInterface *ccd)
{   
    currentCCD = (ISD::CCD *) ccd;

    CCDs.append(currentCCD);

    // Let's just make sure
    //if (currentCCD->hasGuideHead())
    //{
       // guiderCombo->clear();
        guiderCombo->addItem(currentCCD->getDeviceName() + QString(" Guider"));
        //useGuideHead = true;
        checkCCD(0);
    //}

}

void Guide::syncCCDInfo()
{
    INumberVectorProperty * nvp = NULL;

    if (currentCCD == NULL)
        return;

    if (useGuideHead)
        nvp = currentCCD->getBaseDevice()->getNumber("GUIDER_INFO");
    else
        nvp = currentCCD->getBaseDevice()->getNumber("CCD_INFO");

    if (nvp)
    {
        INumber *np = IUFindNumber(nvp, "CCD_PIXEL_SIZE_X");
        if (np)
            ccd_hor_pixel = np->value;

        np = IUFindNumber(nvp, "CCD_PIXEL_SIZE_Y");
        if (np)
            ccd_ver_pixel = np->value;

        np = IUFindNumber(nvp, "CCD_PIXEL_SIZE_Y");
        if (np)
            ccd_ver_pixel = np->value;
    }

    updateGuideParams();
}

void Guide::syncTelescopeInfo()
{
    INumberVectorProperty * nvp = currentTelescope->getBaseDevice()->getNumber("TELESCOPE_INFO");

    if (nvp)
    {
        INumber *np = IUFindNumber(nvp, "GUIDER_APERTURE");

        if (np && np->value != 0)
            aperture = np->value;
        else
        {
            np = IUFindNumber(nvp, "TELESCOPE_APERTURE");
            if (np)
                aperture = np->value;
        }

        np = IUFindNumber(nvp, "GUIDER_FOCAL_LENGTH");
        if (np && np->value != 0)
            focal_length = np->value;
        else
        {
            np = IUFindNumber(nvp, "TELESCOPE_FOCAL_LENGTH");
            if (np)
                focal_length = np->value;
        }
    }

    updateGuideParams();

}

void Guide::updateGuideParams()
{
    if (ccd_hor_pixel != -1 && ccd_ver_pixel != -1 && focal_length != -1 && aperture != -1)
    {
        pmath->set_guider_params(ccd_hor_pixel, ccd_ver_pixel, aperture, focal_length);
        int x,y,w,h;

        if (currentCCD->hasGuideHead() == false)
            useGuideHead = false;

        ISD::CCDChip *targetChip = currentCCD->getChip(useGuideHead ? ISD::CCDChip::GUIDE_CCD : ISD::CCDChip::PRIMARY_CCD);

        if (targetChip == NULL)
        {
            appendLogText(i18n("Connection to the guide CCD is lost."));
            return;
        }

        emit guideChipUpdated(targetChip);

        guider->setTargetChip(targetChip);

        if (targetChip->getFrame(&x,&y,&w,&h))
            pmath->set_video_params(w, h);

        guider->setInterface();

    }
}

void Guide::addST4(ISD::ST4 *newST4)
{
    foreach(ISD::ST4 *guidePort, ST4List)
    {
        if (guidePort == newST4)
            return;
    }

    ST4Combo->addItem(newST4->getDeviceName());
    ST4List.append(newST4);

    ST4Driver = ST4List.at(0);
    ST4Combo->setCurrentIndex(0);

    for (int i=0; i < ST4List.count(); i++)
    {
        if (ST4List.at(i)->getDeviceName() == Options::sT4Driver())
        {
           ST4Driver = ST4List.at(i);
           ST4Combo->setCurrentIndex(i);
           break;
        }
    }

    GuideDriver = ST4Driver;

}

void Guide::setAO(ISD::ST4 *newAO)
{
    AODriver = newAO;
    guider->setAO(true);
}

bool Guide::capture()
{
    if (currentCCD == NULL)
        return false;

    double seqExpose = exposureIN->value();

    ISD::CCDChip *targetChip = currentCCD->getChip(useGuideHead ? ISD::CCDChip::GUIDE_CCD : ISD::CCDChip::PRIMARY_CCD);

    CCDFrameType ccdFrame = FRAME_LIGHT;

    if (currentCCD->isConnected() == false)
    {
        appendLogText(i18n("Error: Lost connection to CCD."));
        return false;
    }

    //If calibrating, reset frame
    if (calibration->getCalibrationStage() == rcalibration::CAL_CAPTURE_IMAGE)
    {
        targetChip->resetFrame();
        guider->setSubFramed(false);
    }

    // Exposure changed, take a new dark
    if (useDarkFrame && darkExposure != seqExpose)
    {
        darkExposure = seqExpose;
        targetChip->setFrameType(FRAME_DARK);

        if (calibration->useAutoStar() == false)
            KMessageBox::information(NULL, i18n("If the guider camera if not equipped with a shutter, cover the telescope or camera in order to take a dark exposure."), i18n("Dark Exposure"), "dark_exposure_dialog_notification");

        connect(currentCCD, SIGNAL(BLOBUpdated(IBLOB*)), this, SLOT(newFITS(IBLOB*)));
        targetChip->capture(seqExpose);

        appendLogText(i18n("Taking a dark frame. "));

        return true;
    }

    targetChip->setCaptureMode(FITS_GUIDE);
    targetChip->setFrameType(ccdFrame);
    if (darkImage == NULL || calibration->useDarkFrame() == false)
        targetChip->setCaptureFilter((FITSScale) filterCombo->currentIndex());

    if (guider->isGuiding())
    {
         if (guider->isRapidGuide() == false)
             connect(currentCCD, SIGNAL(BLOBUpdated(IBLOB*)), this, SLOT(newFITS(IBLOB*)));

         targetChip->capture(seqExpose);
         return true;
    }

    connect(currentCCD, SIGNAL(BLOBUpdated(IBLOB*)), this, SLOT(newFITS(IBLOB*)));
    targetChip->capture(seqExpose);

    return true;

}
void Guide::newFITS(IBLOB *bp)
{
    INDI_UNUSED(bp);

    FITSViewer *fv = currentCCD->getViewer();

    disconnect(currentCCD, SIGNAL(BLOBUpdated(IBLOB*)), this, SLOT(newFITS(IBLOB*)));

    ISD::CCDChip *targetChip = currentCCD->getChip(useGuideHead ? ISD::CCDChip::GUIDE_CCD : ISD::CCDChip::PRIMARY_CCD);

    // Received a dark calibration frame
    if (targetChip->getFrameType() == FRAME_DARK)
    {
        FITSView *targetImage = targetChip->getImage(FITS_CALIBRATE);
        if (targetImage)
        {
            darkImage = targetImage->getImageData();
            capture();
        }
        else
            appendLogText(i18n("Dark frame processing failed."));

       return;
    }

    FITSView *targetImage = targetChip->getImage(FITS_GUIDE);

    if (targetImage == NULL)
    {
        pmath->set_image(NULL);
        guider->setImage(NULL);
        calibration->setImage(NULL);
        return;
    }

    FITSData *image_data = targetImage->getImageData();

    if (image_data == NULL)
        return;

    if (darkImage && darkImage->getImageBuffer() != image_data->getDarkFrame())
    {
        image_data->setDarkFrame(darkImage->getImageBuffer());
        image_data->applyFilter((FITSScale) filterCombo->currentIndex());
        targetImage->rescale(ZOOM_KEEP_LEVEL);
        targetImage->updateFrame();
    }

    pmath->set_image(targetImage);
    guider->setImage(targetImage);

    fv->show();

    // It should be false in case we do not need to process the image for motion
    // which happens when we take an image for auto star selection.
    if (calibration->setImage(targetImage) == false)
        return;



    if (isSuspended)
    {
        //capture();
        return;
    }

    if (guider->isDithering())
    {
        pmath->do_processing();
        if (guider->dither() == false)
        {
            appendLogText(i18n("Dithering failed. Autoguiding aborted."));
            guider->abort();
            emit ditherFailed();
        }
    }
    else if (guider->isGuiding())
    {
        guider->guide();

         if (guider->isGuiding())
            capture();
    }
    else if (calibration->isCalibrating())
    {
        GuideDriver = ST4Driver;
        pmath->do_processing();
        calibration->processCalibration();

         if (calibration->isCalibrationComplete())
         {
             guider->setReady(true);
             tabWidget->setTabEnabled(1, true);
             emit guideReady();
         }
    }

}


void Guide::appendLogText(const QString &text)
{

    logText.insert(0, i18nc("log entry; %1 is the date, %2 is the text", "%1 %2", QDateTime::currentDateTime().toString("yyyy-MM-ddThh:mm:ss"), text));

    emit newLog();
}

void Guide::clearLog()
{
    logText.clear();
    emit newLog();
}

void Guide::setDECSwap(bool enable)
{
    if (ST4Driver == NULL || guider == NULL)
        return;

    guider->setDECSwap(enable);
    ST4Driver->setDECSwap(enable);

}

bool Guide::sendPulse( GuideDirection ra_dir, int ra_msecs, GuideDirection dec_dir, int dec_msecs )
{
    if (GuideDriver == NULL || (ra_dir == NO_DIR && dec_dir == NO_DIR))
        return false;

    if (calibration->isCalibrating())
        QTimer::singleShot( (ra_msecs > dec_msecs ? ra_msecs : dec_msecs) + 100, this, SLOT(capture()));

    return GuideDriver->doPulse(ra_dir, ra_msecs, dec_dir, dec_msecs);
}

bool Guide::sendPulse( GuideDirection dir, int msecs )
{
    if (GuideDriver == NULL || dir==NO_DIR)
        return false;

    if (calibration->isCalibrating())
        QTimer::singleShot(msecs+100, this, SLOT(capture()));

    return GuideDriver->doPulse(dir, msecs);

}

QStringList Guide::getST4Devices()
{
    QStringList devices;

    foreach(ISD::ST4* driver, ST4List)
        devices << driver->getDeviceName();

    return devices;
}

bool Guide::setST4(QString device)
{
    for (int i=0; i < ST4List.count(); i++)
        if (ST4List.at(i)->getDeviceName() == device)
        {
            ST4Combo->setCurrentIndex(i);
            return true;
        }

    return false;
}

void Guide::newST4(int index)
{
    if (ST4List.empty() || index >= ST4List.count())
        return;

    ST4Driver = ST4List.at(index);

    Options::setST4Driver(ST4Driver->getDeviceName());

    GuideDriver = ST4Driver;

}

double Guide::getReticleAngle()
{
    return calibration->getReticleAngle();
}

void Guide::viewerClosed()
{
    pmath->set_image(NULL);
    guider->setImage(NULL);
    calibration->setImage(NULL);
}

void Guide::processRapidStarData(ISD::CCDChip *targetChip, double dx, double dy, double fit)
{
    // Check if guide star is lost
    if (dx == -1 && dy == -1 && fit == -1)
    {
        KMessageBox::error(NULL, i18n("Lost track of the guide star. Rapid guide aborted."));
        guider->abort();
        return;
    }

    FITSView *targetImage = targetChip->getImage(FITS_GUIDE);

    if (targetImage == NULL)
    {
        pmath->set_image(NULL);
        guider->setImage(NULL);
        calibration->setImage(NULL);
    }

    if (rapidGuideReticleSet == false)
    {
        // Let's set reticle parameter on first capture to those of the star, then we check if there
        // is any set
        double x,y,angle;
        pmath->get_reticle_params(&x, &y, &angle);
        pmath->set_reticle_params(dx, dy, angle);
        rapidGuideReticleSet = true;
    }

    pmath->setRapidStarData(dx, dy);

    if (guider->isDithering())
    {
        pmath->do_processing();
        if (guider->dither() == false)
        {
            appendLogText(i18n("Dithering failed. Autoguiding aborted."));
            guider->abort();
            emit ditherFailed();
        }
    }
    else
    {
        guider->guide();
        capture();
    }

}

void Guide::startRapidGuide()
{
    ISD::CCDChip *targetChip = currentCCD->getChip(useGuideHead ? ISD::CCDChip::GUIDE_CCD : ISD::CCDChip::PRIMARY_CCD);

    if (currentCCD->setRapidGuide(targetChip, true) == false)
    {
        appendLogText(i18n("The CCD does not support Rapid Guiding. Aborting..."));
        guider->abort();
        return;
    }

    rapidGuideReticleSet = false;

    pmath->setRapidGuide(true);
    currentCCD->configureRapidGuide(targetChip, true);
    connect(currentCCD, SIGNAL(newGuideStarData(ISD::CCDChip*,double,double,double)), this, SLOT(processRapidStarData(ISD::CCDChip*,double,double,double)));

}

void Guide::stopRapidGuide()
{
    ISD::CCDChip *targetChip = currentCCD->getChip(useGuideHead ? ISD::CCDChip::GUIDE_CCD : ISD::CCDChip::PRIMARY_CCD);

    pmath->setRapidGuide(false);

    rapidGuideReticleSet = false;

    currentCCD->disconnect(SIGNAL(newGuideStarData(ISD::CCDChip*,double,double,double)));

    currentCCD->configureRapidGuide(targetChip, false, false, false);

    currentCCD->setRapidGuide(targetChip, false);

}


void Guide::dither()
{
   if (guider->isDithering() == false)
        guider->dither();
}

void Guide::updateGuideDriver(double delta_ra, double delta_dec)
{
    guideDeviationRA  = delta_ra;
    guideDeviationDEC = delta_dec;

    if (guider->isGuiding() == false)
        return;

    if (guider->isDithering())
    {
        GuideDriver = ST4Driver;
        return;
    }

    // Guide via AO only if guiding deviation is below AO limit
    if (AODriver != NULL && (fabs(delta_ra) < guider->getAOLimit()) && (fabs(delta_dec) < guider->getAOLimit()))
    {
        if (AODriver != GuideDriver)
                appendLogText(i18n("Using %1 to correct for guiding errors.", AODriver->getDeviceName()));
        GuideDriver = AODriver;
        return;
    }

    if (GuideDriver != ST4Driver)
        appendLogText(i18n("Using %1 to correct for guiding errors.", ST4Driver->getDeviceName()));

    GuideDriver = ST4Driver;
}

bool Guide::isCalibrationComplete()
{
    return calibration->isCalibrationComplete();
}

bool Guide::isCalibrationSuccessful()
{
    return calibration->isCalibrationSuccessful();
}

bool Guide::startCalibration()
{
    return calibration->startCalibration();
}

bool Guide::stopCalibration()
{
    return calibration->stopCalibration();
}

bool Guide::isGuiding()
{
    return guider->isGuiding();
}

bool Guide::startGuiding()
{
    return guider->start();
}

bool Guide::stopGuiding()
{
    isSuspended=false;
    return guider->abort(true);
}

void Guide::setSuspended(bool enable)
{
    if (enable == isSuspended || (enable && guider->isGuiding() == false))
        return;

    isSuspended = enable;

    if (isSuspended == false)
        capture();

    if (isSuspended)
        appendLogText(i18n("Guiding suspended."));
    else
        appendLogText(i18n("Guiding resumed."));
}

void Guide::setExposure(double value)
{
    exposureIN->setValue(value);
}


void Guide::setImageFilter(const QString & value)
{
    for (int i=0; i < filterCombo->count(); i++)
        if (filterCombo->itemText(i) == value)
        {
            filterCombo->setCurrentIndex(i);
            break;
        }
}

void Guide::setCalibrationTwoAxis(bool enable)
{
    calibration->setCalibrationTwoAxis(enable);
}

void Guide::setCalibrationAutoStar(bool enable)
{
    calibration->setCalibrationAutoStar(enable);
}

void Guide::setCalibrationAutoSquareSize(bool enable)
{
    calibration->setCalibrationAutoSquareSize(enable);
}

void Guide::setCalibrationDarkFrame(bool enable)
{
    calibration->setCalibrationDarkFrame(enable);
}

void Guide::setCalibrationParams(int boxSize, int pulseDuration)
{
    calibration->setCalibrationParams(boxSize, pulseDuration);
}

void Guide::setGuideBoxSize(int boxSize)
{
     guider->setGuideOptions(boxSize, guider->getAlgorithm(), guider->useSubFrame(), guider->useRapidGuide());
}

void Guide::setGuideAlgorithm(const QString & algorithm)
{
     guider->setGuideOptions(guider->getBoxSize(), algorithm, guider->useSubFrame(), guider->useRapidGuide());
}

void Guide::setGuideSubFrame(bool enable)
{
     guider->setGuideOptions(guider->getBoxSize(), guider->getAlgorithm(), enable , guider->useRapidGuide());
}

void Guide::setGuideRapid(bool enable)
{
     guider->setGuideOptions(guider->getBoxSize(), guider->getAlgorithm(), guider->useSubFrame() , enable);
}

void Guide::setDither(bool enable, double value)
{
    guider->setDither(enable, value);
}

QList<double> Guide::getGuidingDeviation()
{
    QList<double> deviation;

    deviation << guideDeviationRA << guideDeviationDEC;

    return deviation;
}

void Guide::startAutoCalibrateGuiding()
{
    connect(calibration, SIGNAL(calibrationCompleted(bool)), this, SLOT(checkAutoCalibrateGuiding(bool)));
    calibration->startCalibration();
}

void Guide::checkAutoCalibrateGuiding(bool successful)
{
    calibration->disconnect(this);

    if (successful)
    {
        appendLogText(i18n("Auto calibration successful. Starting guiding..."));
        startGuiding();
    }
    else
    {
        appendLogText(i18n("Auto calibration failed."));
    }
}

}


