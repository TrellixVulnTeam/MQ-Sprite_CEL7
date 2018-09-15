#include "projectmodel.h"

#include <QColor>
#include <QDebug>
#include <QPainter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QTextStream>
#include <QSettings>
#include <QImageWriter>
#include <QTemporaryFile>
#include <QDir>
#include <QtZlib/zlib.h>

#include <cstdlib>
#include <ctime>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ios>
#include "tarball.h"

// TODO: Convert old files
static const int PROJECT_SAVE_FILE_VERSION = 1;


bool operator==(const AssetRef& a, const AssetRef& b){
    return (a.uuid.isNull() && b.uuid.isNull()) ||  (a.uuid == b.uuid && a.type == b.type);
}

bool operator!=(const AssetRef& a, const AssetRef& b){
    return !(a==b);
}

bool operator<(const AssetRef& a, const AssetRef& b){
    return a.uuid > b.uuid;
}

uint qHash(const AssetRef &key){
    return qHash(key.uuid);
}

static ProjectModel* sInstance = nullptr;

ProjectModel* PM(){return ProjectModel::Instance();}

ProjectModel::ProjectModel()
{
    sInstance = this;
}

ProjectModel::~ProjectModel(){    
    clear();
}

ProjectModel* ProjectModel::Instance(){
    return sInstance;
}

AssetRef ProjectModel::createAssetRef(){
    AssetRef ref;
    ref.uuid = QUuid::createUuid();
    return ref;
}

Asset* ProjectModel::getAsset(const AssetRef& ref){
    switch (ref.type){
        case AssetType::Part: return getPart(ref);
        case AssetType::Composite: return getComposite(ref);
        case AssetType::Folder: return getFolder(ref);
    }
    return nullptr;
}

bool ProjectModel::hasAsset(const AssetRef& ref){
    return getAsset(ref)!=nullptr;
}

Part* ProjectModel::getPart(const AssetRef& uuid){
    return parts.value(uuid).data();
}


bool ProjectModel::hasPart(const AssetRef& uuid){
    return getPart(uuid)!=nullptr;
}

Composite* ProjectModel::getComposite(const AssetRef& uuid){
    return composites.value(uuid).data();
}

bool ProjectModel::hasComposite(const AssetRef& uuid){
    return getComposite(uuid)!=nullptr;
}

Folder* ProjectModel::getFolder(const AssetRef& uuid){
    return folders.value(uuid).data();
}

bool ProjectModel::hasFolder(const AssetRef& uuid){
    return getFolder(uuid)!=nullptr;
}


Part* ProjectModel::findPartByName(const QString& name){
    for(auto p: parts.values()){
        if (p->name==name){
            return p.data();
        }
    }
    return nullptr;
}

Composite* ProjectModel::findCompositeByName(const QString& name){
    for(auto p: composites.values()){
        if (p->name==name){
            return p.data();
        }
    }
    return nullptr;
}

Folder* ProjectModel::findFolderByName(const QString& name){
    for(auto f: folders.values()){
        if (f->name==name){
            return f.data();
        }
    }
    return nullptr;
}

void ProjectModel::clear(){
    parts.clear();
    composites.clear();
    folders.clear();
    fileName = QString();
}

bool ProjectModel::load(const QString& fileName, QString& reason){	
    std::fstream in(fileName.toStdString().c_str(), std::ios::in | std::ios::binary);
	if (!in.is_open()) {
		reason = "Cannot open file";
		return false;
	}

    lindenb::io::TarIn tarball(in);
    tarball.read();
    if (!tarball.ok()){
		reason = "Cannot read project file";
        return false;
    }

    auto& fileMap = tarball.fileMap();

    if (fileMap.count("data.json") == 0){
		reason = "Internal data.json is missing";
        return false;
    }

    auto& dataRec = fileMap["data.json"];

	int dataLength = 0;
	for (int i = 0; i < dataRec.length; ++i) {
		if (dataRec.buffer[i] == '\0') {
			dataLength = i;
			break;
		}
	}

	if (dataLength == 0) {
		reason = "Internal data.json is empty";
		return false;
	}

    QJsonParseError error;
    QJsonDocument dataDoc = QJsonDocument::fromJson(QByteArray(dataRec.buffer, dataLength), &error);
	
    if (error.error != QJsonParseError::NoError){		
		reason = "Internal data.json parse error: " + error.errorString();		
        return false;
    }
    else if (dataDoc.isNull() || dataDoc.isEmpty() || !dataDoc.isObject()){
		reason = "Internal data.json is not a valid json object";
        return false;
    }

    QJsonObject dataObj = dataDoc.object();
    if (!dataObj.contains("version")){
		reason = "Internal data.json has no version field";
        return false;
    }

    if (dataObj.value("version").toInt(0) != PROJECT_SAVE_FILE_VERSION){
		reason = "Internal data.json has an invalid version";
        return false;
    }

    if (fileMap.count("prefs.json")>0){
        auto& prefsRec = fileMap["prefs.json"];
		int presLength = 0;

		for (int i = 0; i < prefsRec.length; ++i) {
			if (prefsRec.buffer[i] == '\0') {
				presLength = i;
				break;
			}
		}

        QJsonParseError error;
        QJsonDocument prefsDoc = QJsonDocument::fromJson(QByteArray(prefsRec.buffer, presLength), &error);

        if (error.error!=QJsonParseError::NoError){
            qWarning() << "Internal prefs.json parse error: " << error.errorString();
        }
        else if (prefsDoc.isNull() || prefsDoc.isEmpty() || !prefsDoc.isObject()){
			qWarning() << "Internal prefs.json is not a vaid json object";
        }
        else {
            QSettings settings;
            QJsonObject settingsObj = prefsDoc.object();
            for(QJsonObject::iterator it = settingsObj.begin(); it!=settingsObj.end(); it++){
                const QJsonValue& val = it.value();

                if (it.key()=="background_colour"){
                    uint col = val.toString().toUInt();
                    settings.setValue(it.key(), col);
                }
                else {
                    settings.setValue(it.key(), val.toVariant());
                }
            }
        }
    }

    // Load all the images (and store them in an image map)
	// The ownership of these are taken by the sprites when they're loaded
    QMap<QString, QSharedPointer<QImage>> imageMap;   
    for(auto it = tarball.fileMap().begin(); it!=tarball.fileMap().end(); it++){
        QString assetName = QString::fromStdString(it->first);
        if (assetName.endsWith(".png")){
            const auto& record = it->second;
            auto img = QSharedPointer<QImage>::create();
            bool res = img->loadFromData(QByteArray(record.buffer, record.length), "PNG");
			// qDebug() << "Loaded: " << assetName << " " << img->width() << " x " << img->height();
            Q_ASSERT(res);
            imageMap.insert(assetName, img);
        }
    }

    // Load data.json, connecting the Images* too
    {
        QJsonObject folders = dataObj.value("folders").toObject();
        QJsonObject parts = dataObj.value("parts").toObject();
        QJsonObject comps = dataObj.value("comps").toObject();

        if (!folders.isEmpty()){
            for(auto it = folders.begin(); it!=folders.end(); it++){
                const QString& uuid = it.key();
                const QJsonObject& folderObj = it.value().toObject();
                auto folder = QSharedPointer<Folder>::create();
                folder->ref.uuid = QUuid(uuid);
                folder->ref.type = AssetType::Folder;
                JsonToFolder(folderObj, folder.get());
                this->folders.insert(folder->ref, folder);
            }
        }

        if (!parts.isEmpty()){
            for(QJsonObject::iterator it = parts.begin(); it!=parts.end(); it++){
                const QString& uuid = it.key();
                const QJsonObject& partObj = it.value().toObject();
				auto part = QSharedPointer<Part>::create();
                part->ref.uuid = QUuid(uuid);
                part->ref.type = AssetType::Part;
                part->properties = QString();
                JsonToPart(partObj, imageMap, part.get());
                this->parts.insert(part->ref, part);
            }
        }

        if (!comps.isEmpty()){
            for(QJsonObject::iterator it = comps.begin(); it!=comps.end(); it++){
                const QString& uuid = it.key();
                const QJsonObject& compObj = it.value().toObject();
				auto composite = QSharedPointer<Composite>::create();
                composite->ref.uuid = QUuid(uuid);
                composite->ref.type = AssetType::Composite;
                composite->properties = QString();
                JsonToComposite(compObj, composite.get());
                this->composites.insert(composite->ref, composite);
            }
        }
    }

    this->fileName = fileName;
    return true;
}

bool ProjectModel::save(const QString& fileName){
    qDebug() << "TODO: Implement saving of layers ";
    return false;

    /*

    std::fstream out(fileName.toStdString().c_str(), std::ios::out | std::ios::binary);
    if(!out.is_open()){
        qDebug() << "Cannot open out";
        return false;
    }
    QMap<QString,QImage*> imageMap;

    lindenb::io::TarOut tarball(out);

    //////////////////////
    // data.json
    // NB: build the image set while processing data
    //////////////////////

    {
        QJsonObject data;

        // version etc
        data.insert("version", PROJECT_SAVE_FILE_VERSION);

        // folder
        QJsonObject foldersObject;

        QMapIterator<AssetRef, Folder*> fit(this->folders);
        while (fit.hasNext()){
            fit.next();
            QJsonObject folderObject;
            const Folder* f = fit.value();
            FolderToJson(f->name, *f, &folderObject);

            QString& uuid = f->ref.uuid.toString();
            foldersObject.insert(uuid, folderObject);
        }
        data.insert("folders", foldersObject);

        // parts
        QJsonObject partsObject;

        QMapIterator<AssetRef, Part*> pit(this->parts);
        while (pit.hasNext()){
            pit.next();
            QJsonObject partObject;
            const Part* p = pit.value();
            PartToJson(p->name, *p, &partObject, &imageMap);

            QString& uuid = p->ref.uuid.toString();
            partsObject.insert(uuid, partObject);
        }
        data.insert("parts", partsObject);

        // comps
        QJsonObject compsObject;
        {
            QMapIterator<AssetRef, Composite*> it(this->composites);
            while (it.hasNext()){
                it.next();
                const Composite* comp = it.value();
                QString compNameFixed = comp->name;
                compNameFixed.replace(' ','_');
                QJsonObject compObject;
                CompositeToJson(compNameFixed, *comp, &compObject);

                QString uuid = comp->ref.uuid.toString();
                compsObject.insert(uuid, compObject);
            }
        }
        data.insert("comps", compsObject);

        // Send it out
        QString dataStr;
        QTextStream out(&dataStr);
        QJsonDocument doc(data);
        out << doc.toJson();
        tarball.put("data.json", dataStr.toStdString().c_str());
    }

    //////////////////////
    // prefs.json
    //////////////////////

    {
        QJsonObject settingsObj;
        QSettings settings;
        foreach(QString key, settings.allKeys()){
            const QVariant& val = settings.value(key);
            if (key=="background_colour"){
                uint col = val.toUInt();
                settingsObj.insert(key, QString::number(col));
            }
            else {
                settingsObj.insert(key, QJsonValue::fromVariant(val));
            }
        }

        QString prefsdataStr;
        QTextStream prefsout(&prefsdataStr);
        QJsonDocument prefsdoc(settingsObj);
        prefsout << prefsdoc.toJson();

        tarball.put("prefs.json", prefsdataStr.toStdString().c_str());
    }

    ////////////////
    // images.json
    ////////////////

    {
        QDir tempPath = QDir(QDir::tempPath());
        QString tempFileName = tempPath.absoluteFilePath("tmp.png");

        QMapIterator<QString,QImage*> it(imageMap);
        while (it.hasNext()){
            it.next();

            const QImage* img = it.value();
            if (img){
                bool res = img->save(tempFileName, "PNG");
                if (res){
                    tarball.putFile(tempFileName.toStdString().c_str(),it.key().toStdString().c_str());
                }
                else {
                    qDebug() << "Couldn't save image: " << it.key();
                }
            }
        }
    }

    tarball.finish();
    out.close();

    this->fileName = fileName;
    return true;
    */
}

void ProjectModel::JsonToFolder(const QJsonObject& obj, Folder* folder){
    // Load name
    folder->name = obj["name"].toString();

    if (obj.contains("parent")){
        folder->parent.uuid = QUuid(obj["parent"].toString());
        folder->parent.type = AssetType::Folder;
    }
}

void ProjectModel::FolderToJson(const QString& name, const Folder& folder, QJsonObject* obj){

    obj->insert("name", name);
    if (!folder.parent.isNull()){
        obj->insert("parent", folder.parent.uuid.toString());
    }
}

void ProjectModel::JsonToPart(const QJsonObject& obj, const QMap<QString,QSharedPointer<QImage>>& imageMap, Part* part){
    part->name = obj["name"].toString();

    if (obj.contains("parent")){
        part->parent.uuid = QUuid(obj["parent"].toString());
        part->parent.type = AssetType::Folder;
    }

    for(auto it = obj.begin(); it != obj.end(); it++){        
        const QString& modeName = it.key();
        
		if (modeName == "properties"){
            part->properties = it.value().toString();
            continue;
        }

        const QJsonObject& modeObject = it.value().toObject();
        if (!modeObject.isEmpty()){
            Part::Mode m;

            m.width = modeObject.value("width").toInt();
            m.height = modeObject.value("height").toInt();
            m.numFrames = modeObject.value("numFrames").toInt();
            m.numPivots = modeObject.value("numPivots").toInt();
            m.framesPerSecond = modeObject.value("framesPerSecond").toInt();
			
            const QJsonArray& frameArray = modeObject.value("frames").toArray();
            Q_ASSERT(frameArray.count()==m.numFrames);
            for(int frame=0;frame<frameArray.count();frame++){
                const QJsonObject& frameObject = frameArray.at(frame).toObject();

                int ax = frameObject.value("ax").toVariant().toInt();
                int ay = frameObject.value("ay").toVariant().toInt();
                m.anchor.push_back(QPoint(ax,ay));

                QString imageName = frameObject.value("image").toString();
                auto image = imageMap.value(imageName);
				int imageHeight = image->height();
				int imageWidth = image->width();

                Q_ASSERT(image);
                Q_ASSERT(imageWidth == m.width && imageHeight == m.height);
				m.frames.push_back(image);
                for(int p=0;p<m.numPivots;p++){
                    int px = frameObject.value(QString("p%1x").arg(p)).toVariant().toInt();
                    int py = frameObject.value(QString("p%1y").arg(p)).toVariant().toInt();
                    m.pivots[p].push_back(QPoint(px,py));
                }
                for(int p=m.numPivots;p<MAX_PIVOTS;p++){
                    m.pivots[p].push_back(QPoint(0,0));
                }
            }

            part->modes.insert(modeName, m);
        }
    }
}

void ProjectModel::PartToJson(const QString& name, const Part& part, QJsonObject* obj, QMap<QString, QSharedPointer<QImage>>* imageMap){
    qDebug() << "Support layers";

    /*
    if (!part.properties.isEmpty()){
        obj->insert("properties", part.properties);
    }


    QMapIterator<QString,Part::Mode> mit(part.modes);
    QString partNameFixed = name;
    partNameFixed.replace(' ', '_');
    obj->insert("name", name);

    if (!part.parent.isNull()){
        obj->insert("parent", part.parent.uuid.toString());
    }

    while (mit.hasNext()){
        mit.next();

        const Part::Mode& m = mit.value();
        QString modeNameFixed = mit.key();
        modeNameFixed.replace(' ', '_');

        // part, mode..
        QJsonObject modeObject;
        modeObject.insert("width", m.width);
        modeObject.insert("height", m.height);
        modeObject.insert("numFrames", m.numFrames);
        modeObject.insert("numPivots", m.numPivots);
        modeObject.insert("framesPerSecond", m.framesPerSecond);

        QJsonArray frameArray;
        for(int frame=0;frame<m.numFrames;frame++){
            QJsonObject frameObject;
            frameObject.insert("ax", m.anchor.at(frame).x());
            frameObject.insert("ay", m.anchor.at(frame).y());

            QString frameNum = QString("%1").arg(frame, 3, 10, QChar('0')).toUpper();
            QString imageName = QString("%1_%2_%3.png").arg(partNameFixed,modeNameFixed,frameNum);

            imageMap->insert(imageName, m.images.at(frame));
            frameObject.insert("image", imageName);

            // pivots
            for(int p=0;p<m.numPivots;p++){
                frameObject.insert(QString("p%1x").arg(p), m.pivots[p].at(frame).x());
                frameObject.insert(QString("p%1y").arg(p), m.pivots[p].at(frame).y());
            }

            frameArray.append(frameObject);
        }

        modeObject.insert("frames", frameArray);
        obj->insert(mit.key(), modeObject);
    }
    */
}

void ProjectModel::CompositeToJson(const QString& name, const Composite& comp, QJsonObject* obj){
    obj->insert("root", comp.root);
    obj->insert("properties", comp.properties);
    obj->insert("name", name);

    if (!comp.parent.isNull()){
        obj->insert("parent", comp.parent.uuid.toString());
    }

    QJsonArray compChildren;
    foreach(const QString& childName, comp.children){
        QString fixedChildName = childName;
        fixedChildName.replace(' ','_');

        const Composite::Child& child = comp.childrenMap.value(childName);
        QJsonObject childObject;
        childObject.insert("name", fixedChildName);
        childObject.insert("parent", child.parent);
        childObject.insert("parentPivot", child.parentPivot);
        childObject.insert("z", child.z);
        childObject.insert("part", child.part.uuid.toString());

        QJsonArray children;
        foreach(int ci, child.children){
            children.append(ci);
        }
        childObject.insert("children", children);

        compChildren.push_back(childObject);
        // compChildren.insert(fixedChildName, childObject);
    }
    obj->insert("parts", compChildren);
}

void ProjectModel::JsonToComposite(const QJsonObject& obj, Composite* comp){
    comp->root = obj.value("root").toVariant().toInt();    
    comp->name = obj["name"].toString();
    comp->properties = obj.value("properties").toString();

    if (obj.contains("parent")){
        comp->parent.uuid = QUuid(obj["parent"].toString());
        comp->parent.type = AssetType::Folder;
    }

    QJsonArray children = obj.value("parts").toArray();
    int index = 0;
    foreach(const QJsonValue& value, children){
        QJsonObject childObject = value.toObject();
        QString name = childObject.value("name").toString();
        comp->children.push_back(name);

        Composite::Child child;
        child.parent = childObject.value("parent").toVariant().toInt();
        child.parentPivot = childObject.value("parentPivot").toVariant().toInt();
        child.z = childObject.value("z").toVariant().toInt();
        child.part.uuid = QUuid(childObject.value("part").toString());
        child.part.type = AssetType::Part;
        child.index = index;

        QJsonArray childrenOfChild = childObject.value("children").toArray();
        foreach(const QJsonValue& ci, childrenOfChild){
            child.children.push_back(ci.toVariant().toInt());
        }
        comp->childrenMap.insert(name, child);

        index++;
    }
}
