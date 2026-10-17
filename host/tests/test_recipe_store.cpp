#include <gtest/gtest.h>

#include <limits>
#include <memory>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>

#include "mc/recipe.hpp"
#include "mc_host/recipe_store.hpp"

// 配方库（SQLite）的用例。
//
// 重点不在"能存能取"—— 那个写错了会立刻发现。重点是两条**边界上的行为**：
//   ① 同名同版本不能再存一份（配方管理的规矩）；
//   ② 库里的行被改动过时，装载要能说出来，而不是把它当成一份正常配方。
namespace {

using mchost::RecipeStore;

mc::Recipe makeValid() {
    mc::Recipe recipe;
    recipe.name = "WAFER-A1";
    recipe.version = 3U;
    recipe.mode = 1U;
    recipe.target = 200.0F;
    recipe.kp = 1.6F;
    recipe.ki = 0.4F;
    recipe.kd = 0.02F;
    return recipe;
}

// 从**另一条连接**改库：模拟"有人手工改了表/库被写坏"。
// 用本应用的写入路径改是没意义的 —— 那条路会把校验和一起算对。
void tamperColumn(const QString& dbPath, const QString& sql, const QVariant& value,
                  const QString& name) {
    const QString connection = QStringLiteral("mc_test_tamper");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(dbPath);
        ASSERT_TRUE(db.open()) << db.lastError().text().toStdString();
        QSqlQuery query(db);
        query.prepare(sql);
        query.addBindValue(value);
        query.addBindValue(name);
        ASSERT_TRUE(query.exec()) << query.lastError().text().toStdString();
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

class RecipeStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(dir_->isValid());
        path_ = dir_->filePath(QStringLiteral("recipes.db"));
        ASSERT_TRUE(store_.open(path_)) << store_.error().toStdString();
    }

    qint64 insertOrDie(const mc::Recipe& recipe) {
        qint64 id = 0;
        QString error;
        EXPECT_TRUE(store_.insert(recipe, &id, error)) << error.toStdString();
        return id;
    }

    std::unique_ptr<QTemporaryDir> dir_;
    QString path_;
    RecipeStore store_;
};

TEST_F(RecipeStoreTest, SavingAndListingRoundTripsEveryField) {
    const mc::Recipe recipe = makeValid();
    const qint64 id = insertOrDie(recipe);
    ASSERT_GT(id, 0);

    QString error;
    const std::vector<RecipeStore::Row> rows = store_.list(error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(rows.size(), 1U);

    EXPECT_EQ(rows[0].id, id);
    EXPECT_EQ(rows[0].recipe.name, recipe.name);
    EXPECT_EQ(rows[0].recipe.version, recipe.version);
    EXPECT_EQ(rows[0].recipe.mode, recipe.mode);
    EXPECT_FLOAT_EQ(rows[0].recipe.target, recipe.target);
    EXPECT_FLOAT_EQ(rows[0].recipe.kp, recipe.kp);
    EXPECT_FLOAT_EQ(rows[0].recipe.ki, recipe.ki);
    EXPECT_FLOAT_EQ(rows[0].recipe.kd, recipe.kd);
    // 刚写进去的行必然和记下来的校验和一致。
    EXPECT_FALSE(rows[0].corrupted);
    EXPECT_EQ(rows[0].storedChecksum, mc::recipeChecksum(recipe));
}

TEST_F(RecipeStoreTest, TheSameNameAndVersionCannotBeStoredTwice) {
    const qint64 id = insertOrDie(makeValid());
    ASSERT_GT(id, 0);

    QString error;
    qint64 second = 0;
    EXPECT_FALSE(store_.insert(makeValid(), &second, error));
    EXPECT_FALSE(error.isEmpty()) << "拒绝必须带原因，否则操作员不知道该改版本号还是改名字";

    // 大小写不同也算同一份：同一个人不会认为 WAFER-A1 与 wafer-a1 是两个配方，
    // 而两份并存的结果是列表里出现两条看起来一样的记录，其中只有一条是对的。
    mc::Recipe lowercase = makeValid();
    lowercase.name = "wafer-a1";
    EXPECT_FALSE(store_.insert(lowercase, &second, error));

    // 版本号不同才是"新的一版"，必须放行。
    mc::Recipe nextVersion = makeValid();
    nextVersion.version = 4U;
    EXPECT_TRUE(store_.insert(nextVersion, &second, error)) << error.toStdString();

    EXPECT_EQ(store_.list(error).size(), 2U);
}

TEST_F(RecipeStoreTest, AnEditFromOutsideShowsUpAsCorruptedInsteadOfLoadingSilently) {
    ASSERT_GT(insertOrDie(makeValid()), 0);

    tamperColumn(path_, QStringLiteral("UPDATE recipes SET target = ? WHERE name = ?"), 999.0,
                 QStringLiteral("WAFER-A1"));

    QString error;
    const std::vector<RecipeStore::Row> rows = store_.list(error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(rows.size(), 1U);

    // 行**照样列出来**：藏掉它等于"库里少了一份配方"，而真相是"有一份被改过"。
    EXPECT_TRUE(rows[0].corrupted);
    EXPECT_FLOAT_EQ(rows[0].recipe.target, 999.0F);
    EXPECT_NE(rows[0].storedChecksum, mc::recipeChecksum(rows[0].recipe));
}

TEST_F(RecipeStoreTest, TouchingOnlyTheNameIsAlsoCaught) {
    // 校验和必须盖住全部字段。只盖参数的话，"名字被改了但校验和没变"
    // 就是一条验不出来的改动 —— 而名字恰恰是操作员挑配方的依据。
    ASSERT_GT(insertOrDie(makeValid()), 0);
    tamperColumn(path_, QStringLiteral("UPDATE recipes SET name = ? WHERE name = ?"),
                 QStringLiteral("TAMPERED"), QStringLiteral("WAFER-A1"));

    QString error;
    const std::vector<RecipeStore::Row> rows = store_.list(error);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_TRUE(rows[0].corrupted);
}

TEST_F(RecipeStoreTest, AnInvalidRecipeIsRefusedAtTheStoreBoundary) {
    // 库是边界：写进来的不止界面一条路。而"库里躺着一条非法配方"这件事
    // 没有任何合理的处置方式，所以只能在门口拒掉。
    mc::Recipe bad = makeValid();
    bad.name.clear();

    QString error;
    qint64 id = 0;
    EXPECT_FALSE(store_.insert(bad, &id, error));
    EXPECT_FALSE(error.isEmpty());

    bad = makeValid();
    bad.version = 0U;
    EXPECT_FALSE(store_.insert(bad, &id, error));

    bad = makeValid();
    bad.target = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(store_.insert(bad, &id, error));

    EXPECT_TRUE(store_.list(error).empty()) << "被拒的配方一条都不该落库";
}

TEST_F(RecipeStoreTest, UpdateCannotStealAnotherRowsVersion) {
    const mc::Recipe first = makeValid();
    ASSERT_GT(insertOrDie(first), 0);

    mc::Recipe second = makeValid();
    second.name = "WAFER-B7";
    const qint64 secondId = insertOrDie(second);
    ASSERT_GT(secondId, 0);

    // 把 B7 改成 A1 v3：占用了别人的名字+版本 → 必须拒，而且不能改坏原有的两份。
    mc::Recipe clash = second;
    clash.name = first.name;
    clash.version = first.version;

    QString error;
    EXPECT_FALSE(store_.update(secondId, clash, error));
    EXPECT_FALSE(error.isEmpty());

    const std::vector<RecipeStore::Row> rows = store_.list(error);
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].recipe.name, "WAFER-A1");
    EXPECT_EQ(rows[1].recipe.name, "WAFER-B7");

    // 自己改自己不冲突（版本号没变，还是它自己那一行）。
    mc::Recipe sameId = second;
    sameId.target = 111.0F;
    EXPECT_TRUE(store_.update(secondId, sameId, error)) << error.toStdString();

    // 更新也要重算校验和，否则"改完之后这一行立刻是坏的"。
    const std::vector<RecipeStore::Row> after = store_.list(error);
    ASSERT_EQ(after.size(), 2U);
    EXPECT_FALSE(after[1].corrupted);
    EXPECT_FLOAT_EQ(after[1].recipe.target, 111.0F);
}

TEST_F(RecipeStoreTest, RemovingDeletesOnlyTheSelectedRow) {
    ASSERT_GT(insertOrDie(makeValid()), 0);
    mc::Recipe other = makeValid();
    other.name = "WAFER-B7";
    const qint64 otherId = insertOrDie(other);
    ASSERT_GT(otherId, 0);

    QString error;
    ASSERT_TRUE(store_.remove(otherId, error)) << error.toStdString();
    const std::vector<RecipeStore::Row> rows = store_.list(error);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].recipe.name, "WAFER-A1");
}

TEST_F(RecipeStoreTest, AnUnopenablePathFailsWithAReason) {
    RecipeStore store;
    EXPECT_FALSE(store.open(QStringLiteral("/definitely/not/here/recipes.db")));
    EXPECT_FALSE(store.isOpen());
    // 打不开必须说得出来为什么：配方库悄悄打不开，用户看到的就是"我的配方没了"。
    EXPECT_FALSE(store.error().isEmpty());
    EXPECT_TRUE(store.error().contains(QStringLiteral("打不开"))) << store.error().toStdString();
}

}  // namespace
